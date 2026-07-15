#!/usr/bin/env bash
# tests/integration/test_vm_lock_cross_unlock.sh
#
# CMP-1: VM 락 교차 unlock 차단 결정적 E2E (실/격리 데몬)
# ---------------------------------------------------------------------------
# 검증 대상: handler_vm_lifecycle.c 의 vm_action_callback 이 ctx->holds_lock
# 이 TRUE 일 때만 unlock_vm_operation(ctx->vm_id) 를 호출한다(HEAD 06631e7).
# 락을 획득하는 유일한 op 는 stop(holds_lock=TRUE) 뿐이고, pause/resume/limit 은
# 같은 콜백을 타지만 락을 획득하지 않으므로(holds_lock=FALSE) 콜백에서 남의
# 락(예: 동시 vm.delete 의 DELETING 락)을 지우면 안 된다.
#   - unlock_vm_operation 은 vm_id 만으로 삭제한다:
#     DELETE FROM vm_locks WHERE vm_id = ?  (vm_state.c ~608)
#     → 수정 전에는 limit 콜백이 이 삭제를 무조건 실행해 교차 unlock 이 가능했다.
#
# 결정적 재현(실제 레이스/실 VM 불요):
#   1. 격리 데몬 기동(vm_locks DB 는 /var/lib/purecvisor/vm_state.db, bwrap bind
#      로 임시 디렉터리에 shadow). init_pending_state_machine(main.c:667)이 생성.
#   2. DELETING 락 행을 DB 에 직접 INSERT(op_type=3, pid=1=alive, locked_at=now)
#      → 동시 vm.delete 가 락을 보유한 상태를 시뮬레이션. (WAL 로 데몬에 라이브 반영.)
#   3. 같은 vm_id 로 vm.limit 발화. 도메인 부재로 워커는 실패하지만(test:/// 드라이버)
#      콜백은 성공/실패 무관하게 실행 — 이것이 검증 대상 코드 경로. limit 은
#      holds_lock=FALSE 라 콜백이 unlock 을 호출하지 않아야 한다.
#   4. [CMP-1 핵심] SELECT count(*) ... vm_id='<testvm>' == 1 (DELETING 락 잔존).
#      수정 전이면 0(교차 삭제됨).
#   5. [직렬화] 락 잔존 중 같은 vm_id 로 vm.stop(=lock_vm_operation 호출) → DELETING
#      락 충돌로 거부되어야(destroy/create 경합 미발생). vm.create 는 ZFS/virt-install
#      인프라가 필요해 미구동 — 브리프 허용대로 동일한 lock_vm_operation 게이트를
#      타는 vm.stop 로 직렬화를 실증한다.
#   6. [무회귀] 락 없는 다른 vm_id 로 vm.stop → 콜백(holds_lock=TRUE)이 정상 해제
#      → count == 0. stop 은 핸들러에서 락 획득 → 콜백에서 해제까지 결정적으로 탄다.
#
# 격리 방식(SEC-2 test_sec2_bootstrap_fallback.sh 관습 재사용):
#   - bubblewrap(bwrap) 사용자 네임스페이스 uid-0 맵핑, /var/lib/purecvisor 와
#     /etc/purecvisor 를 mktemp 임시 디렉터리로 bind → 프로덕션 DB/소켓 완전 shadow.
#   - libvirt 는 PCV_LIBVIRT_URI=test:///default (인메모리 mock, 호스트 무접촉)로
#     고정 — 도메인 조회는 즉시 not-found, 프로덕션 libvirt 를 절대 건드리지 않는다.
#   - UDS 직결은 ADMIN 로 취급(dispatcher.c:520)되어 토큰 불요. 소켓은 bind 된
#     /var/lib/purecvisor/daemon.sock (호스트 $STATE/var-lib/daemon.sock).
#
# 전제조건 부재 시(bwrap/sqlite3/python3/userns 불가) 깨끗이 SKIP(exit 0).
#
# 실행: bash tests/integration/test_vm_lock_cross_unlock.sh
# 부작용: 없음(모든 상태는 mktemp -d 임시 디렉터리, 종료 시 정리).

set -uo pipefail

GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS=0; FAIL=0
pass() { echo -e "${GREEN}[PASS]${NC} $*"; PASS=$((PASS+1)); }
fail() { echo -e "${RED}[FAIL]${NC} $*"; FAIL=$((FAIL+1)); }
info() { echo -e "${CYAN}[INFO]${NC} $*"; }
note() { echo -e "${YELLOW}[NOTE]${NC} $*"; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
DAEMON_BIN="$REPO/bin/purecvisorsd"

skip() { echo -e "${YELLOW}[SKIP]${NC} CMP-1 E2E: $*"; exit 0; }

# ── 전제조건 검사 ────────────────────────────────────────────────
command -v bwrap    >/dev/null 2>&1 || skip "bwrap(bubblewrap) 미설치"
command -v sqlite3  >/dev/null 2>&1 || skip "sqlite3 미설치"
command -v python3  >/dev/null 2>&1 || skip "python3 미설치"
command -v setsid   >/dev/null 2>&1 || skip "setsid 미설치"

# 비특권 userns + uid 0 맵핑 가능 여부
if ! bwrap --unshare-user --uid 0 --gid 0 --ro-bind / / --dev /dev --proc /proc \
        /bin/true >/dev/null 2>&1; then
    skip "비특권 사용자 네임스페이스(uid-map) 불가 — 이 호스트에서 격리 데몬 기동 불가"
fi

# 데몬 바이너리 (없으면 빌드 시도)
if [ ! -x "$DAEMON_BIN" ]; then
    info "데몬 바이너리 없음 — make daemon 시도"
    make -C "$REPO" daemon >/dev/null 2>&1 || true
fi
[ -x "$DAEMON_BIN" ] || skip "데몬 바이너리 빌드 실패 ($DAEMON_BIN)"

# ── 빈 포트 선택 (rest_port 용; 검증은 UDS 로 진행) ────────────────
pick_free_port() {
    local p
    for p in 28082 29082 31082 33082 37082 41082 43082; do
        if ! ss -ltn 2>/dev/null | grep -qE ":${p}([[:space:]]|$)"; then
            echo "$p"; return 0
        fi
    done
    return 1
}
PORT="$(pick_free_port)" || skip "빈 포트 확보 실패"

# ── 격리 상태 디렉터리 ────────────────────────────────────────────
STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-cmp1.XXXXXX")"
mkdir -p "$STATE/var-lib" "$STATE/etc"
DB="$STATE/var-lib/vm_state.db"
SOCK="$STATE/var-lib/daemon.sock"

# 테스트 vm_id (프로덕션 도메인과 충돌 불가한 유니크 이름)
SUF="$$-$RANDOM"
VM_LOCKED="cmp1-locked-$SUF"    # DELETING 락 수동 보유
VM_REGRESS="cmp1-regress-$SUF"  # 무회귀: stop 정상 해제

# DELETING 락에 기록할 PID. 격리 데몬은 bwrap userns 안에서 uid 0 로 맵핑되지만
# 호스트 real uid 는 1000 이다. pid_is_alive() 는 kill(pid,0)==0 로 판정하는데,
# host PID 1(real root systemd)을 kill 하면 EPERM 이라 "dead" 로 오판 → 락을 고아로
# 덮어써 직렬화 검증이 무력화된다. 그래서 데몬과 같은 real uid(1000)이며 테스트
# 내내 살아있는 이 스크립트의 PID($$)를 쓴다 — 데몬이 kill($$,0)==0 로 alive 판정.
LOCK_PID="$$"

# ── UDS JSON-RPC 클라이언트 (응답 후 서버가 소켓 close → recv EOF) ──
cat > "$STATE/uds_call.py" <<'PYEOF'
import socket, sys
sock_path, payload = sys.argv[1], sys.argv[2]
try:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(8.0)
    s.connect(sock_path)
    s.sendall(payload.encode())
    buf = b""
    while True:
        try:
            chunk = s.recv(4096)
        except socket.timeout:
            break
        if not chunk:
            break
        buf += chunk
    s.close()
    sys.stdout.write(buf.decode(errors="replace"))
except Exception as e:
    sys.stderr.write("uds_call error: %s\n" % e)
    sys.exit(2)
PYEOF

uds_call() {  # $1 = json payload  → stdout: response json
    python3 "$STATE/uds_call.py" "$SOCK" "$1" 2>/dev/null
}

sq() {  # sqlite helper with busy_timeout (WAL 동시 접근 안전)
    # .timeout 은 값을 출력하지 않는다(PRAGMA busy_timeout 은 5000 을 출력해 결과 오염).
    sqlite3 -cmd ".timeout 5000" "$DB" "$1" 2>/dev/null
}

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

write_conf() {
    cat > "$STATE/etc/daemon.conf" <<EOF
[daemon]
socket_path = /var/lib/purecvisor/daemon.sock
rest_port = $PORT
admin_user = admin
admin_password = Cmp1E2eFixedPw-not-for-prod
jwt_secret = cmp1-e2e-fixed-secret-not-for-prod-0001
libvirt_uri = test:///default
log_level = info
drain_timeout = 1
EOF
}

boot() {  # 격리 데몬 기동 후 UDS 소켓 응답 대기
    setsid bwrap \
        --unshare-user --uid 0 --gid 0 \
        --ro-bind / / \
        --dev /dev \
        --proc /proc \
        --tmpfs /tmp \
        --bind "$STATE/var-lib" /var/lib/purecvisor \
        --bind "$STATE/etc" /etc/purecvisor \
        --setenv PCV_LIBVIRT_URI test:///default \
        --chdir / \
        "$DAEMON_BIN" > "$STATE/daemon.log" 2>&1 < /dev/null &
    echo "$!" > "$STATE/bwrap.pid"
    local i resp
    for i in $(seq 1 60); do
        if [ -S "$SOCK" ]; then
            resp="$(uds_call '{"jsonrpc":"2.0","method":"vm.list","params":{},"id":"ping"}')"
            case "$resp" in
                *'"jsonrpc"'*|*'"result"'*|*'"error"'*) return 0 ;;
            esac
        fi
        sleep 0.5
    done
    return 1
}

echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  CMP-1 VM 락 교차 unlock 차단 결정적 E2E            ${NC}"
echo -e "${CYAN}  binary=$DAEMON_BIN${NC}"
echo -e "${CYAN}  state=$STATE  uds=$SOCK${NC}"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"

# ═══════════════════════════════════════════════════════════════
# 단계 1: 격리 데몬 기동 + vm_state.db(vm_locks) 준비 확인
# ═══════════════════════════════════════════════════════════════
write_conf
if ! boot; then
    fail "S1: 격리 데몬이 UDS 응답 실패"
    echo "---- daemon.log (마지막 30줄) ----"; tail -30 "$STATE/daemon.log" 2>/dev/null
    echo -e "\n${RED}데몬 기동 실패로 중단${NC}"; exit 1
fi
# vm_locks 테이블 준비 확인
tbl="$(sq "SELECT name FROM sqlite_master WHERE type='table' AND name='vm_locks';")"
if [ "$tbl" = "vm_locks" ]; then
    pass "S1: 격리 데몬 기동 + vm_locks 테이블 준비 ($DB)"
else
    fail "S1: vm_locks 테이블 부재 (tbl='$tbl')"
    echo "---- daemon.log (마지막 20줄) ----"; tail -20 "$STATE/daemon.log" 2>/dev/null
    exit 1
fi

# ═══════════════════════════════════════════════════════════════
# 단계 2: DELETING 락 수동 보유 재현 (동시 vm.delete 시뮬레이션)
#   op_type=3(VM_OP_DELETING), pid=$$(데몬이 alive 판정 가능), locked_at=now
# ═══════════════════════════════════════════════════════════════
NOW="$(date +%s)"
sq "INSERT INTO vm_locks (vm_id, op_type, pid, locked_at) VALUES ('$VM_LOCKED', 3, $LOCK_PID, $NOW);"
row="$(sq "SELECT vm_id || '|' || op_type || '|' || pid FROM vm_locks WHERE vm_id='$VM_LOCKED';")"
if [ "$row" = "$VM_LOCKED|3|$LOCK_PID" ]; then
    pass "S2: DELETING 락 보유 ($row) — op_type 3 = VM_OP_DELETING, pid=$LOCK_PID(alive)"
else
    fail "S2: DELETING 락 INSERT 실패 (row='$row')"
    exit 1
fi

# ═══════════════════════════════════════════════════════════════
# 단계 3+4 (CMP-1 핵심): vm.limit 발화 → DELETING 락 잔존 확인
#   limit 은 lock 을 획득하지 않음(holds_lock=FALSE) → 콜백이 unlock 미실행.
#   워커는 도메인 부재로 실패하나 콜백은 실행됨(검증 대상 경로).
# ═══════════════════════════════════════════════════════════════
lim_resp="$(uds_call "{\"jsonrpc\":\"2.0\",\"method\":\"vm.limit\",\"params\":{\"vm_id\":\"$VM_LOCKED\",\"cpu\":50},\"id\":\"limit1\"}")"
info "S3: vm.limit 응답 = ${lim_resp:-<empty>}"
sleep 1.5   # 워커 + 콜백 완료 대기
cnt_core="$(sq "SELECT count(*) FROM vm_locks WHERE vm_id='$VM_LOCKED';")"
if [ "$cnt_core" = "1" ]; then
    pass "S4(CMP-1 핵심): vm.limit 후 DELETING 락 잔존 (count=$cnt_core) — 교차 unlock 차단"
else
    fail "S4(CMP-1 핵심): vm.limit 후 count=$cnt_core (기대 1) — 락이 교차 삭제됨(수정 미적용/회귀)"
fi

# ═══════════════════════════════════════════════════════════════
# 단계 5 (직렬화): 락 잔존 중 vm.stop(락 획득 op) → 거부 확인
#   vm.create 대체: 동일 lock_vm_operation 게이트를 타는 vm.stop 로 실증.
# ═══════════════════════════════════════════════════════════════
stop_resp="$(uds_call "{\"jsonrpc\":\"2.0\",\"method\":\"vm.stop\",\"params\":{\"vm_id\":\"$VM_LOCKED\"},\"id\":\"stop-blocked\"}")"
info "S5: vm.stop(락 보유 VM) 응답 = ${stop_resp:-<empty>}"
sleep 0.5
cnt_after_stop="$(sq "SELECT count(*) FROM vm_locks WHERE vm_id='$VM_LOCKED';")"
# 거부 판정: 에러 응답(locked/DELETING 언급) AND DELETING 락 그대로 잔존
rejected=0
case "$stop_resp" in
    *'"error"'*) rejected=1 ;;
esac
locked_word=0
case "$stop_resp" in
    *locked*|*DELETING*) locked_word=1 ;;
esac
if [ "$rejected" = "1" ] && [ "$locked_word" = "1" ] && [ "$cnt_after_stop" = "1" ]; then
    pass "S5(직렬화): vm.stop 이 DELETING 락 충돌로 거부됨 + 락 잔존 (count=$cnt_after_stop)"
elif [ "$cnt_after_stop" = "1" ]; then
    # 락은 잔존하나 거부 문구가 예상과 다를 때: 부분 실증으로 표기(교차 삭제는 아님)
    note "S5: DELETING 락은 잔존(count=1)하나 거부 응답 문구 확인 불가 — 응답='$stop_resp'"
    fail "S5(직렬화): vm.stop 거부 응답 문구 미확인 (rejected=$rejected locked_word=$locked_word)"
else
    fail "S5(직렬화): vm.stop 후 DELETING 락 count=$cnt_after_stop (기대 1) — 직렬화 붕괴"
fi

# ═══════════════════════════════════════════════════════════════
# 단계 6 (무회귀): 락 없는 다른 VM 에 vm.stop → 정상 해제(count=0)
#   stop 은 핸들러에서 lock_vm_operation(STOPPING) 획득(holds_lock=TRUE),
#   콜백에서 unlock 으로 해제. 도메인 부재라도 락 라이프사이클은 완전 실행.
# ═══════════════════════════════════════════════════════════════
pre_regress="$(sq "SELECT count(*) FROM vm_locks WHERE vm_id='$VM_REGRESS';")"
reg_resp="$(uds_call "{\"jsonrpc\":\"2.0\",\"method\":\"vm.stop\",\"params\":{\"vm_id\":\"$VM_REGRESS\"},\"id\":\"stop-regress\"}")"
info "S6: vm.stop(무락 VM, 사전 count=$pre_regress) 응답 = ${reg_resp:-<empty>}"
sleep 1.5   # 워커 + 콜백(unlock) 완료 대기
cnt_regress="$(sq "SELECT count(*) FROM vm_locks WHERE vm_id='$VM_REGRESS';")"
if [ "$pre_regress" = "0" ] && [ "$cnt_regress" = "0" ]; then
    pass "S6(무회귀): stop 이 STOPPING 락을 정상 해제 (완료 후 count=$cnt_regress)"
else
    fail "S6(무회귀): stop 후 count=$cnt_regress (기대 0, 사전=$pre_regress) — 락 미해제 회귀"
fi

# DELETING 락은 여전히 잔존해야(무락 op 들이 못 지웠음) — 최종 스냅샷
final_locked="$(sq "SELECT count(*) FROM vm_locks WHERE vm_id='$VM_LOCKED';")"
info "최종: DELETING 락(count=$final_locked) / 무회귀 VM 락(count=$cnt_regress)"

# ═══════════════════════════════════════════════════════════════
# 정리
# ═══════════════════════════════════════════════════════════════
kill_daemon
rm -f "$STATE/bwrap.pid"

echo ""
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
printf "  결과: ${GREEN}PASS %d${NC} / ${RED}FAIL %d${NC}\n" "$PASS" "$FAIL"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
[ "$FAIL" -eq 0 ]
