#!/usr/bin/env bash
# tests/integration/test_vm_batch_fanout.sh
#
# ADR-0025(반사실 검증) 효과-테스트 — vm.batch 프로덕션 핸들러 (팬아웃)
# ---------------------------------------------------------------------------
# 이 테스트가 지키는 것: `vm.batch` 핸들러가 **실제로 VM 별로 팬아웃**한다
# (옛 스텁의 "무동작 accepted"가 아님). 옛 스텁(dispatcher.c:3923)은 action 을
# 각 VM 에 수행하지 않고 **모든** 입력 VM(미존재 포함)을 그냥 "accepted" 로
# 반환했다 — reject/whitelist 로직이 전무했다 ("보고성공 무동작", ADR-0025 가
# 겨냥하는 클래스). Task 3 이 이를 whitelist 팬아웃으로 교체했다:
#   - action ∈ {start,stop} 만 허용(vm_manager 에 `_async` public fn 실존) —
#     그 외는 -32602 "unsupported batch action".
#   - 존재하는 각 VM 에만 purecvisor_vm_manager_<action>_async(mgr, vm) 직접
#     호출(fire-and-forget 팬아웃), 미존재 VM 은 rejected[{vm,"VM not found"}].
#   - 1개 집계 응답 {action, accepted[], rejected[]}.
#
# 반사실(counterfactual) — 옛 스텁을 되돌리면 RED 가 되는 단언:
#   S2  미존재 VM 이 accepted 가 아니라 rejected[]("VM not found") 에 든다.
#       (스텁은 미존재도 accepted 에 넣고 rejected 는 항상 비었다 → RED)
#   S3  action="pause"(whitelist 밖) → error -32602 "unsupported batch action".
#       (스텁은 whitelist 자체가 없어 pause 도 accepted 반환 → RED)
#   S5  팬아웃이 실제 dispatch 되어 각 VM 이 libvirt 상태전이(running→shutoff)를
#       일으킨다 — stop 워커가 virDomainGetState 로 VIR_DOMAIN_SHUTOFF 를 관측해
#       daemon.log 에 "VM '<vm>' shut down gracefully" 를 남긴다.
#       (스텁은 action_fn 을 한 번도 호출하지 않아 워커 로그가 0줄 → RED)
#   S6  팬아웃 accepted VM 각각이 완료 콜백에서 per-VM 'vm.stop' 감사 행을 남긴다.
#       (옛 NULL 콜백[M-1 이전]은 개별 결과를 audit 에 안 남겨 0행 → RED)
#   ※ 모든 단언은 **실 프로덕션 핸들러**를 UDS 로 구동해 관측한다(스텁 재구현이
#     아님). 스텁 거동("accepted 만, reject/whitelist/dispatch/audit 전무")으로
#     되돌리면 S2/S3/S5/S6 이 실제로 RED 가 된다 — S6 은 감사 DB 행 수라는
#     구체 관측으로 그 반사실을 inject-and-observe 형태로 고정한다.
#
# 왜 custom test 토폴로지(3 도메인) 인가:
#   libvirt test 드라이버 test:///default 는 도메인이 "test" 1개뿐이라
#   "VM 별 팬아웃"을 N>1 로 실증하기 약하다. test://<abs.xml> 형식으로 running
#   도메인 3개(batch-vm-a/b/c)를 정의한 노드 XML 을 로드하면 매 연결이 동일
#   토폴로지를 본다(연결마다 독립 인메모리 — 아래 .50 이연 참고).
#
# 실-VM 상태전이의 관측 범위(.50 이연 경계):
#   test 드라이버는 **연결별 인메모리** 세계다 — 매 virConnectOpen("test://…")
#   가 pristine(3 도메인 running) 사본을 얻는다. vm_manager stop 워커는
#   virt_conn_pool_acquire() 로 **자기** 풀 연결 세계에서 stop 을 수행하므로,
#   그 전이는 **워커 자신**의 virDomainGetState 로는 관측되지만(→ S4 daemon.log
#   증거) 호스트의 `virsh -c test:// domstate` 나 별도 vm.list RPC(다른 풀 연결
#   세계)로는 **결정적으로 재조회할 수 없다**. 따라서:
#     - 로컬 완결: 팬아웃 응답(accepted/rejected/whitelist) + 워커가 관측한
#       실 상태전이 로그 증거(S4).
#     - `.50` 이연: 실 libvirt(공유 상태 하이퍼바이저)에서 배치 후 독립 조회
#       (virsh/vm.list)로 각 VM 의 최종 상태를 **교차 재조회**하는 E2E.
#       (여기서 재조회 단언을 조작하지 않는다 — 브리프: "Do NOT fake a state
#       assertion". S4 는 조작이 아니라 데몬 자신의 진짜 런타임 로그다.)
#
# 격리 방식(SEC-2/DISP-1 harness 관습 재사용 — test_snapshot_verify_effect.sh):
#   - bubblewrap(bwrap) 사용자 네임스페이스 uid-0 맵핑, /var/lib/purecvisor 와
#     /etc/purecvisor 를 mktemp 임시 디렉터리로 bind → 프로덕션 DB/소켓 shadow.
#   - libvirt 는 test://<격리 노드 XML>(인메모리 mock, 호스트 무접촉).
#   - UDS 소켓은 격리 상태 디렉터리 안에 생성 → 호스트에서 nc -U 로 직결.
#     UDS 는 로컬 신뢰 경로라 인증 불요(UDS-direct = ADMIN, dispatcher.c:521).
#   - CLI 배선(pcvctl vm batch <start|stop> <vm...>)은 Task 5 — 여기서는 다른
#     통합 테스트처럼 raw JSON-RPC 를 UDS 로 직접 보낸다(send_rpc).
#
# 전제조건 부재 시(bwrap/nc/setsid/python3/userns 불가) 깨끗이 SKIP(exit 0).
# 데몬이 부팅했으나 검증이 실패하면 FAIL(exit 1).
#
# 실행: bash tests/integration/test_vm_batch_fanout.sh
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
# PCV_DAEMON_BIN 오버라이드 허용(.50 배포 데모용). 기본은 로컬 빌드.
DAEMON_BIN="${PCV_DAEMON_BIN:-$REPO/bin/purecvisorsd}"

skip() { echo -e "${YELLOW}[SKIP]${NC} vm.batch 팬아웃 효과-테스트: $*"; exit 0; }

# ── 전제조건 검사 ────────────────────────────────────────────────
command -v bwrap   >/dev/null 2>&1 || skip "bwrap(bubblewrap) 미설치"
command -v nc      >/dev/null 2>&1 || skip "nc(netcat) 미설치"
command -v setsid  >/dev/null 2>&1 || skip "setsid 미설치"
command -v python3 >/dev/null 2>&1 || skip "python3 미설치(응답 JSON 파싱에 필요)"

# 비특권 userns + uid 0 맵핑 가능 여부
if ! bwrap --unshare-user --uid 0 --gid 0 --ro-bind / / --dev /dev --proc /proc \
        /bin/true >/dev/null 2>&1; then
    skip "비특권 사용자 네임스페이스(uid-map) 불가 — 이 호스트에서 격리 데몬 기동 불가"
fi

# 데몬 바이너리 (없으면 빌드 시도 — make single → bin/purecvisorsd)
if [ ! -x "$DAEMON_BIN" ]; then
    info "데몬 바이너리 없음 — make single 시도"
    make -C "$REPO" single >/dev/null 2>&1 || true
fi
[ -x "$DAEMON_BIN" ] || skip "데몬 바이너리 빌드 실패 ($DAEMON_BIN)"

# ── 테스트 픽스처 (VM 이름) ───────────────────────────────────────
VM_A='batch-vm-a'; VM_B='batch-vm-b'; VM_C='batch-vm-c'
GHOST='batch-vm-ghost'   # 토폴로지에 없는 VM → rejected 예상

# ── 격리 상태 디렉터리 ────────────────────────────────────────────
STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-vmbatch.XXXXXX")"
mkdir -p "$STATE/var-lib" "$STATE/etc"
SOCK="$STATE/var-lib/daemon.sock"          # 컨테이너 /var/lib/purecvisor/daemon.sock 호스트측
AUDIT_DB="$STATE/var-lib/pcv_audit.db"     # 데몬 감사 DB(호스트 가시) — M-1 per-VM audit 반사실용
DLOG="$STATE/daemon.log"

# ── test 드라이버 노드 토폴로지(running 도메인 3개) ────────────────
# 컨테이너 /etc/purecvisor/topo.xml 로 노출(아래 --bind $STATE/etc). libvirt
# test 드라이버 URI 는 test://<absolute-path> — 매 연결이 이 토폴로지를 본다.
cat > "$STATE/etc/topo.xml" <<XML
<node>
  <domain type="test">
    <name>${VM_A}</name>
    <memory>65536</memory>
    <os><type>hvm</type></os>
  </domain>
  <domain type="test">
    <name>${VM_B}</name>
    <memory>65536</memory>
    <os><type>hvm</type></os>
  </domain>
  <domain type="test">
    <name>${VM_C}</name>
    <memory>65536</memory>
    <os><type>hvm</type></os>
  </domain>
</node>
XML
LIBVIRT_URI='test:///etc/purecvisor/topo.xml'

# ── 데몬 설정 (UDS 소켓을 격리 디렉터리 안으로) ───────────────────
cat > "$STATE/etc/daemon.conf" <<EOF
[daemon]
socket_path = /var/lib/purecvisor/daemon.sock
libvirt_uri = ${LIBVIRT_URI}
rest_port = 0
admin_user = admin
admin_password = VmBatchFanout-not-for-prod
jwt_secret = vmbatch-fanout-fixed-secret-not-for-prod-0001
log_level = info
drain_timeout = 1
EOF

# ── UDS JSON-RPC 헬퍼 (다른 통합 테스트와 동일 관습) ──────────────
send_rpc() { echo "$1" | nc -U "$SOCK" 2>/dev/null || true; }

# ── 응답 파서 (python3) ───────────────────────────────────────────
# check_batch_resp <resp> <accepted-csv> <rejected-csv>
#   accepted-csv 의 각 이름이 result.accepted 에 존재 &&
#   rejected-csv 의 각 이름이 result.rejected 의 vm 이고 reason 에 "VM not found" &&
#   rejected-csv 의 각 이름이 accepted 에 **없음** → exit 0, 아니면 exit 1(+진단 stderr)
check_batch_resp() {
    RESP="$1" WANT_ACC="$2" WANT_REJ="$3" python3 - <<'PY'
import json, os, sys
resp = os.environ["RESP"]
want_acc = [x for x in os.environ["WANT_ACC"].split(",") if x]
want_rej = [x for x in os.environ["WANT_REJ"].split(",") if x]
try:
    d = json.loads(resp)
except Exception as e:
    print(f"JSON 파싱 실패: {e} :: {resp!r}", file=sys.stderr); sys.exit(1)
res = d.get("result")
if not isinstance(res, dict):
    print(f"result 객체 없음 (error={d.get('error')})", file=sys.stderr); sys.exit(1)
acc = res.get("accepted") or []
rej = res.get("rejected") or []
rej_names = { (r.get("vm") if isinstance(r, dict) else None) for r in rej }
ok = True
for v in want_acc:
    if v not in acc:
        print(f"accepted 에 '{v}' 없음: {acc}", file=sys.stderr); ok = False
for v in want_rej:
    if v in acc:
        print(f"'{v}' 가 accepted 에 잘못 포함(스텁 회귀 의심): {acc}", file=sys.stderr); ok = False
    match = [r for r in rej if isinstance(r, dict) and r.get("vm") == v]
    if not match:
        print(f"rejected 에 '{v}' 없음: {rej}", file=sys.stderr); ok = False
    elif "VM not found" not in (match[0].get("reason") or ""):
        print(f"'{v}' rejected reason 이 'VM not found' 아님: {match[0]}", file=sys.stderr); ok = False
sys.exit(0 if ok else 1)
PY
}

# check_error_resp <resp> <code> <msg-substr>
check_error_resp() {
    RESP="$1" WANT_CODE="$2" WANT_MSG="$3" python3 - <<'PY'
import json, os, sys
resp = os.environ["RESP"]
want_code = int(os.environ["WANT_CODE"]); want_msg = os.environ["WANT_MSG"]
try:
    d = json.loads(resp)
except Exception as e:
    print(f"JSON 파싱 실패: {e} :: {resp!r}", file=sys.stderr); sys.exit(1)
err = d.get("error")
if not isinstance(err, dict):
    print(f"error 객체 없음 (result={d.get('result')})", file=sys.stderr); sys.exit(1)
if err.get("code") != want_code:
    print(f"error.code {err.get('code')} != {want_code}", file=sys.stderr); sys.exit(1)
if want_msg not in (err.get("message") or ""):
    print(f"error.message 에 '{want_msg}' 없음: {err.get('message')!r}", file=sys.stderr); sys.exit(1)
sys.exit(0)
PY
}

# ── 데몬 라이프사이클 ─────────────────────────────────────────────
kill_daemon() {  # graceful then SIGKILL (세션 그룹 kill, pkill -f 미사용)
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

boot() {  # 격리 데몬 기동 후 UDS 소켓 준비 + 프로브 응답 대기
    setsid bwrap \
        --unshare-user --uid 0 --gid 0 \
        --ro-bind / / \
        --dev /dev \
        --proc /proc \
        --tmpfs /tmp \
        --bind "$STATE/var-lib" /var/lib/purecvisor \
        --bind "$STATE/etc" /etc/purecvisor \
        --setenv PCV_LIBVIRT_URI "$LIBVIRT_URI" \
        --setenv PURECVISOR_LIBVIRT_URI "$LIBVIRT_URI" \
        --chdir / \
        "$DAEMON_BIN" > "$DLOG" 2>&1 < /dev/null &
    echo "$!" > "$STATE/bwrap.pid"

    local i
    # 1) 소켓 파일 생성 대기
    for i in $(seq 1 60); do
        [ -S "$SOCK" ] && break
        sleep 0.5
    done
    [ -S "$SOCK" ] || return 1
    # 2) 프로브 RPC 가 응답할 때까지 대기(디스패처 준비)
    for i in $(seq 1 20); do
        local p; p="$(send_rpc '{"jsonrpc":"2.0","method":"vm.list","params":{},"id":"probe"}')"
        [ -n "$p" ] && return 0
        sleep 0.5
    done
    return 1
}

echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  ADR-0025 효과-테스트: vm.batch 팬아웃                ${NC}"
echo -e "${CYAN}  binary=$DAEMON_BIN${NC}"
echo -e "${CYAN}  state=$STATE${NC}"
echo -e "${CYAN}  libvirt=$LIBVIRT_URI (running: $VM_A,$VM_B,$VM_C)${NC}"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"

# ═══════════════════════════════════════════════════════════════
# 단계 1: 격리 데몬 기동 + UDS 소켓 서빙 확인 (+ 토폴로지 로드)
# ═══════════════════════════════════════════════════════════════
if ! boot; then
    fail "S1: 격리 데몬이 UDS 소켓 프로브에 응답 실패"
    echo "---- daemon.log (마지막 30줄) ----"; tail -30 "$DLOG" 2>/dev/null
    note "로컬에서 데몬 부팅이 불가하면(libvirt 등) 이 테스트는 .50 E2E 서버 대상이다."
    echo -e "\n${RED}데몬 기동 실패로 중단${NC}"; exit 1
fi
pass "S1: 격리 데몬 기동 + UDS 소켓($SOCK) 프로브 응답 (서빙 확인)"

# ═══════════════════════════════════════════════════════════════
# 단계 2 (반사실 핵심 — accept/reject 판별): stop [a,b,c,ghost]
#   accepted = {a,b,c}, rejected = {ghost:"VM not found"}, ghost ∉ accepted.
#   옛 스텁은 미존재 ghost 도 accepted 에 넣고 rejected 는 비웠다 → RED.
#   (이 stop 이 a/b/c 워커 팬아웃도 트리거 → S4 로 상태전이 증거 수집.)
# ═══════════════════════════════════════════════════════════════
BATCH_STOP="{\"jsonrpc\":\"2.0\",\"method\":\"vm.batch\",\"params\":{\"action\":\"stop\",\"vms\":[\"$VM_A\",\"$VM_B\",\"$VM_C\",\"$GHOST\"]},\"id\":\"b1\"}"
RESP_STOP="$(send_rpc "$BATCH_STOP")"
info "S2: stop 배치 응답 = ${RESP_STOP}"
if check_batch_resp "$RESP_STOP" "$VM_A,$VM_B,$VM_C" "$GHOST"; then
    pass "S2(반사실): accepted={$VM_A,$VM_B,$VM_C}, rejected={$GHOST:'VM not found'} — 미존재는 accept 안 됨(스텁이면 RED)"
else
    fail "S2(반사실): accept/reject 판별 실패 (스텁 회귀 의심) — 위 진단 참고"
fi

# ═══════════════════════════════════════════════════════════════
# 단계 3 (반사실 — whitelist): action="pause"(whitelist 밖) → -32602
#   옛 스텁은 whitelist 가 없어 pause 도 accepted 반환 → RED.
# ═══════════════════════════════════════════════════════════════
BATCH_PAUSE="{\"jsonrpc\":\"2.0\",\"method\":\"vm.batch\",\"params\":{\"action\":\"pause\",\"vms\":[\"$VM_A\"]},\"id\":\"b2\"}"
RESP_PAUSE="$(send_rpc "$BATCH_PAUSE")"
info "S3: pause 배치 응답 = ${RESP_PAUSE}"
if check_error_resp "$RESP_PAUSE" "-32602" "unsupported batch action"; then
    pass "S3(반사실): action=pause → error -32602 'unsupported batch action' (스텁이면 accepted 반환이라 RED)"
else
    fail "S3(반사실): pause 가 -32602 'unsupported batch action' 을 반환하지 않음 — 위 진단 참고"
fi

# ═══════════════════════════════════════════════════════════════
# 단계 4 (계약 — 파라미터 검증): vms 누락 → -32602
#   whitelist/reject 와 달리 스텁 판별은 아니지만 핸들러 계약을 고정한다.
# ═══════════════════════════════════════════════════════════════
BATCH_NOVMS='{"jsonrpc":"2.0","method":"vm.batch","params":{"action":"stop"},"id":"b3"}'
RESP_NOVMS="$(send_rpc "$BATCH_NOVMS")"
info "S4: vms 누락 응답 = ${RESP_NOVMS}"
if check_error_resp "$RESP_NOVMS" "-32602" "action and vms"; then
    pass "S4(계약): vms 누락 → error -32602 'action and vms[] required'"
else
    fail "S4(계약): vms 누락이 -32602 를 반환하지 않음 — 위 진단 참고"
fi

# ═══════════════════════════════════════════════════════════════
# 단계 5 (실 dispatch + 상태전이 증거): S2 의 stop 팬아웃이 각 accepted VM 에
#   대해 워커를 실제로 dispatch 했고, 워커가 libvirt 상태전이(running→shutoff)를
#   관측했다. stop_vm_thread_impl 은 virDomainShutdown 후 폴링에서
#   VIR_DOMAIN_SHUTOFF 를 보면 daemon.log 에
#   "VM '<vm>' shut down gracefully" (PCV_LOG_INFO, fflush(stderr) 즉시반영)
#   를 남긴다. 옛 스텁은 action_fn 을 호출조차 안 해 이 줄이 0개 → RED.
#   ※ test 드라이버는 virDomainShutdown 을 동기 전이(→shut off)로 처리하므로
#     첫 폴(≈1s)에서 graceful 로그가 뜬다. 넉넉히 최대 15s 폴링.
# ═══════════════════════════════════════════════════════════════
seen_all_transitions() {
    local v
    for v in "$VM_A" "$VM_B" "$VM_C"; do
        grep -F "$v" "$DLOG" 2>/dev/null | grep -Fq "shut down gracefully" || return 1
    done
    return 0
}
TRANS_OK=0
for i in $(seq 1 30); do   # 30 × 0.5s = 최대 15s
    if seen_all_transitions; then TRANS_OK=1; break; fi
    sleep 0.5
done
if [ "$TRANS_OK" -eq 1 ]; then
    pass "S5(실 전이/dispatch 증거): $VM_A/$VM_B/$VM_C 각각 stop 워커가 VIR_DOMAIN_SHUTOFF 관측 후 'shut down gracefully' 기록 — 실제 팬아웃 dispatch + 상태전이(스텁이면 0줄이라 RED)"
else
    fail "S5(실 전이/dispatch 증거): 일부 VM 의 stop 워커 상태전이 로그 부재 (팬아웃 미dispatch 의심)"
    echo "---- daemon.log: 'shut down'/'destroy' 라인 ----"
    grep -E "shut down|destroy|batch-vm-" "$DLOG" 2>/dev/null | tail -20 || echo "(관련 로그 없음)"
fi

# ═══════════════════════════════════════════════════════════════
# 단계 6 (M-1 반사실 — per-VM 감사): S2 의 stop 팬아웃이 accepted VM 각각에 대해
#   완료 콜백(_vm_batch_action_callback)에서 per-VM 감사 행을 남긴다. 옛 NULL
#   콜백은 개별 결과를 audit 에 전혀 남기지 않았다(배치 집계 응답만) → 그때는
#   'vm.stop' 행이 0이라 이 단언이 RED. 완료 콜백은 워커 g_task_return 후
#   메인루프에서 실행되므로 잠깐 폴링한다.
# ═══════════════════════════════════════════════════════════════
if command -v sqlite3 >/dev/null 2>&1; then
    stop_rows=0
    for i in $(seq 1 20); do
        stop_rows="$(sqlite3 "$AUDIT_DB" "SELECT COUNT(DISTINCT target) FROM audit_log WHERE method='vm.stop' AND target IN ('$VM_A','$VM_B','$VM_C')" 2>/dev/null || echo 0)"
        [ "${stop_rows:-0}" -ge 3 ] && break
        sleep 0.25
    done
    info "S6: audit(vm.stop, accepted VM) distinct target 수 = ${stop_rows}"
    if [ "${stop_rows:-0}" -ge 3 ]; then
        pass "S6(M-1 반사실): 팬아웃 accepted 3 VM 각각 per-VM 'vm.stop' 감사 기록 — 옛 NULL 콜백이면 0행이라 RED"
    else
        fail "S6(M-1 반사실): per-VM 'vm.stop' 감사 행 부족(distinct target=$stop_rows, 기대≥3) — M-1 콜백 미배선 의심"
    fi
else
    note "S6 per-VM 감사 반사실 건너뜀 — sqlite3 미설치(팬아웃/전이 단언 S2~S5 는 유효)"
fi

note ".50 이연: 각 VM 의 최종 상태를 배치와 **독립한** 조회(virsh/vm.list)로"
note "          교차 재조회하는 실-VM E2E 는 실 libvirt(공유 상태) 필요 → .50 배포 게이트."
note "          (test 드라이버는 연결별 인메모리라 로컬 교차 재조회는 비결정적 — S5 는"
note "           워커 자신의 진짜 전이 관측 로그로 dispatch+전이를 결정적으로 고정한다.)"

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
