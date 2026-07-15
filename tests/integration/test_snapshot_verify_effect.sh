#!/usr/bin/env bash
# tests/integration/test_snapshot_verify_effect.sh
#
# ADR-0025(반사실 검증) 효과-테스트 — backup.snapshot.verify 프로덕션 핸들러
# ---------------------------------------------------------------------------
# 이 테스트가 지키는 것: `backup.snapshot.verify` 핸들러가 **실제** `zfs` 출력을
# 반영한다(옛 스텁의 하드코딩 `exists:true`가 아님). 옛 스텁(dispatcher.c:4038)은
# zfs 명령 문자열만 조립하고 `(void)check` 후 무조건 `exists:true`를 반환했다
# ("보고성공 무동작", ADR-0025가 겨냥하는 클래스). Task 1이 이를 async-result
# 핸들러로 교체해 GTask 워커가 `zfs list -t snapshot -H -o name <snap>`를
# pcv_spawn_sync(argv, 셸 미경유)로 실행하고 종료코드로 존재 여부를 판정한다.
# 존재하면 R1 정련이 `zfs get -H -o value written <snap>` property-read 로 뒷받침한다
# (성공→integrity:verified, 실패→degraded; 미존재→missing).
#
# 반사실(counterfactual):
#   - **미존재 스냅샷 → `exists:false`, integrity:missing** — 옛 스텁은 미존재에도
#     `exists:true`였으므로 이 단언이 스텁을 죽인다(하드코딩 true 로 되돌리면 RED).
#   - **존재하나 property-read 실패 → integrity:degraded** — "존재==verified"
#     단순화로 되돌리면 이 단언이 RED(R1 property-read 분기가 살아있음을 고정).
#   - **snapshot param 누락 → 감사 result='fail'**(I-1) — 이 async-result 메서드가
#     g_async_methods 미등록이면 디스패처가 dispatch 시점 무조건 'ok'(에러도 'ok')를
#     남긴다. 그 버그로 되돌리면 fail 행이 0이라 S6 이 RED("보고성공 무동작" 재발 차단).
#
# 왜 mock zfs 인가:
#   pcv_spawn_sync는 전역 GSubprocessLauncher 싱글턴을 쓰며 자식 PATH를
#   `/usr/sbin:/usr/bin:/sbin:/bin`으로 **강제**한다(pcv_spawn.c:148, override=TRUE).
#   따라서 mock `zfs`는 강제 PATH의 첫 디렉터리 `/usr/sbin/zfs`에 bind-mount로
#   그림자를 덮어 사용한다(zfs 호스트의 실 바이너리 위치이자 PATH 최우선).
#   mock 은 두 서브커맨드를 처리한다: `list`(존재-판정) + `get`(property-read).
#   세 픽스처 EXISTING(list0+get0)·MISSING(list1)·DEGRADED(list0+get1) 로
#   integrity 세 분기 verified/missing/degraded 를 모두 재현한다.
#
# 격리 방식(SEC-2/DISP-1 harness 관습 재사용 — test_json_ingress_disp1.sh):
#   - bubblewrap(bwrap) 사용자 네임스페이스 uid-0 맵핑, /var/lib/purecvisor 와
#     /etc/purecvisor 를 mktemp 임시 디렉터리로 bind → 프로덕션 DB/소켓 완전 shadow.
#   - libvirt 는 test:///default (인메모리 mock, 호스트 하이퍼바이저 무접촉).
#   - UDS 소켓은 격리 상태 디렉터리 안에 생성 → 호스트에서 nc -U 로 직결.
#     UDS 는 로컬 신뢰 경로라 인증 불요(다른 SAFE 통합 테스트와 동일).
#   - CLI 배선(pcvctl snapshot verify)은 Task 5 — 아직 없음. 여기서는 다른
#     통합 테스트처럼 raw JSON-RPC 를 UDS 로 직접 보낸다(send_rpc).
#
# 전제조건 부재 시(bwrap/nc/setsid/userns 불가) 깨끗이 SKIP(exit 0).
# 데몬이 부팅했으나 검증이 실패하면 FAIL(exit 1).
#
# 실행: bash tests/integration/test_snapshot_verify_effect.sh
# 부작용: 없음(모든 상태는 mktemp -d 임시 디렉터리, 종료 시 정리).
#
# .50 E2E 참고: 이 효과-테스트는 mock zfs 로 로컬 완결된다(실 ZFS 풀 불요).
#   실-ZFS 스냅샷 대상 검증이 필요하면 .50 E2E 서버에서 PCV_DAEMON_BIN 지정 후
#   동일 스크립트를 돌릴 수 있으나, 로컬 PASS 가 프로덕션 핸들러 계약을 이미 고정한다.

set -uo pipefail

GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS=0; FAIL=0
pass() { echo -e "${GREEN}[PASS]${NC} $*"; PASS=$((PASS+1)); }
fail() { echo -e "${RED}[FAIL]${NC} $*"; FAIL=$((FAIL+1)); }
info() { echo -e "${CYAN}[INFO]${NC} $*"; }
note() { echo -e "${YELLOW}[NOTE]${NC} $*"; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
# PCV_DAEMON_BIN 오버라이드 허용(.50 배포 + 반사실 스텁 데모용). 기본은 로컬 빌드.
DAEMON_BIN="${PCV_DAEMON_BIN:-$REPO/bin/purecvisorsd}"

skip() { echo -e "${YELLOW}[SKIP]${NC} snapshot.verify 효과-테스트: $*"; exit 0; }

# ── 전제조건 검사 ────────────────────────────────────────────────
command -v bwrap  >/dev/null 2>&1 || skip "bwrap(bubblewrap) 미설치"
command -v nc     >/dev/null 2>&1 || skip "nc(netcat) 미설치"
command -v setsid >/dev/null 2>&1 || skip "setsid 미설치"

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

# ── 스냅샷 이름 (테스트 고정 픽스처) ──────────────────────────────
EXISTING_SNAP='tank/pcvtest/vm@verify-exists'                 # list 0 + get 0 → integrity:verified
MISSING_SNAP='tank/pcvtest/vm@verify-no-such-snapshot'        # list 1        → integrity:missing
DEGRADED_SNAP='tank/pcvtest/vm@verify-degraded'               # list 0 + get 1 → integrity:degraded

# ── 격리 상태 디렉터리 ────────────────────────────────────────────
STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-snapverify.XXXXXX")"
mkdir -p "$STATE/var-lib" "$STATE/etc" "$STATE/mockbin"
SOCK="$STATE/var-lib/daemon.sock"          # 컨테이너 /var/lib/purecvisor/daemon.sock 의 호스트측 경로
MOCK_LOG="$STATE/var-lib/zfs_mock.log"     # mock zfs 호출 증거(호스트 가시)
AUDIT_DB="$STATE/var-lib/pcv_audit.db"     # 데몬 감사 DB(호스트 가시) — I-1 반사실용(audit 정확성)

# ── mock zfs — 'list'(존재-판정) + 'get'(integrity property-read) 처리 ─
# 핸들러(R1)는 (1) zfs list 로 존재를 판정하고, 존재하면 (2) zfs get -H -o
# value written <snap> 로 property-read 를 뒷받침한다(성공→verified, 실패→
# degraded). mock 은 세 픽스처로 세 분기를 모두 재현한다:
#   EXISTING → list 0 + get 0(→verified) · MISSING → list 1(→missing) ·
#   DEGRADED → list 0 + get 1(→degraded). 이름은 생성 시 스크립트에 박아
#   넣는다(런처가 강제하는 PATH만 상속되므로 env 불요).
cat > "$STATE/mockbin/zfs" <<MOCKEOF
#!/bin/sh
# mock zfs (효과-테스트 전용). 컨테이너 /usr/sbin/zfs 로 bind 됨.
# 호출 argv 를 기록해 프로덕션 핸들러가 실제로 zfs 를 spawn 했음을 증거로 남긴다.
printf 'MOCKZFS %s\n' "\$*" >> /var/lib/purecvisor/zfs_mock.log 2>/dev/null || true
last=""
for a in "\$@"; do last="\$a"; done   # 스냅샷 이름은 두 서브커맨드 모두 마지막 인자
case "\$1" in
  list)   # 존재-판정: EXISTING/DEGRADED → 존재(0), 그 외 → 미존재(1)
    case "\$last" in
      "${EXISTING_SNAP}"|"${DEGRADED_SNAP}") printf '%s\n' "\$last"; exit 0 ;;
      *) exit 1 ;;
    esac ;;
  get)    # integrity property-read: EXISTING 만 성공(0→verified), DEGRADED 는 실패(1→degraded)
    case "\$last" in
      "${EXISTING_SNAP}") printf '512K\n'; exit 0 ;;
      *) exit 1 ;;
    esac ;;
  *) exit 2 ;;
esac
MOCKEOF
chmod +x "$STATE/mockbin/zfs"

# ── 데몬 설정 (UDS 소켓을 격리 디렉터리 안으로) ───────────────────
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

# ── UDS JSON-RPC 헬퍼 (다른 통합 테스트와 동일 관습) ──────────────
send_rpc() { echo "$1" | nc -U "$SOCK" 2>/dev/null || true; }

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
    # mock zfs 는 강제 PATH 첫 디렉터리 /usr/sbin/zfs 로 그림자를 덮는다.
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
echo -e "${CYAN}  ADR-0025 효과-테스트: backup.snapshot.verify        ${NC}"
echo -e "${CYAN}  binary=$DAEMON_BIN${NC}"
echo -e "${CYAN}  state=$STATE${NC}"
echo -e "${CYAN}  mock zfs → /usr/sbin/zfs (강제 PATH 최우선)${NC}"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"

# ═══════════════════════════════════════════════════════════════
# 단계 1: 격리 데몬 기동 + UDS 소켓 서빙 확인
# ═══════════════════════════════════════════════════════════════
if ! boot; then
    fail "S1: 격리 데몬이 UDS 소켓 프로브에 응답 실패"
    echo "---- daemon.log (마지막 30줄) ----"; tail -30 "$STATE/daemon.log" 2>/dev/null
    note "로컬에서 데몬 부팅이 불가하면(libvirt/zfs-pool 등) 이 테스트는 .50 E2E 서버 대상이다."
    echo -e "\n${RED}데몬 기동 실패로 중단${NC}"; exit 1
fi
pass "S1: 격리 데몬 기동 + UDS 소켓($SOCK) 프로브 응답 (서빙 확인)"

# ═══════════════════════════════════════════════════════════════
# 단계 2: 존재+property-read 성공 → exists:true, integrity:verified
#   (list exit 0 → 존재; get exit 0 → property-read 성공. R1 배선 실증.)
# ═══════════════════════════════════════════════════════════════
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

# ═══════════════════════════════════════════════════════════════
# 단계 3 (반사실 핵심): 미존재 스냅샷 → exists:false  (mock zfs exit 1)
#   옛 스텁은 무조건 exists:true 였다. 이 단언이 "보고성공 무동작"을 죽인다.
# ═══════════════════════════════════════════════════════════════
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

# ═══════════════════════════════════════════════════════════════
# 단계 4 (R1 degraded 반사실): 존재하나 property-read 실패 → integrity:degraded
#   list exit 0(존재) 이지만 get exit 1(property-read 실패). "존재==verified"
#   단순화로 되돌리면 이 단언이 RED — degraded 분기가 살아있음을 고정한다.
# ═══════════════════════════════════════════════════════════════
RESP_DEGR="$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"backup.snapshot.verify\",\"params\":{\"snapshot\":\"${DEGRADED_SNAP}\"},\"id\":\"vd\"}")"
info "S4: degraded 스냅샷 응답 = ${RESP_DEGR}"
if echo "$RESP_DEGR" | grep -Eq '"exists"[[:space:]]*:[[:space:]]*true' \
   && echo "$RESP_DEGR" | grep -Eq '"integrity"[[:space:]]*:[[:space:]]*"degraded"'; then
    pass "S4(반사실): 존재+property-read 실패 → exists:true, integrity:degraded (R1: exists 이나 get exit≠0 분기)"
else
    fail "S4(반사실): degraded 분기 미반영 — 존재==verified 단순화 회귀 의심 (resp='$RESP_DEGR')"
fi

# ═══════════════════════════════════════════════════════════════
# 단계 5 (실 spawn 증거): mock zfs 가 정확한 argv 로 호출되었는가
#   핸들러가 우연히 값을 맞춘 게 아니라 실제로 zfs list + get 을 실행했음을 고정한다.
# ═══════════════════════════════════════════════════════════════
if [ -f "$MOCK_LOG" ] \
   && grep -Fq "list -t snapshot -H -o name ${EXISTING_SNAP}" "$MOCK_LOG" \
   && grep -Fq "list -t snapshot -H -o name ${MISSING_SNAP}" "$MOCK_LOG" \
   && grep -Fq "get -H -o value written ${EXISTING_SNAP}" "$MOCK_LOG"; then
    pass "S5: mock zfs 가 'list -t snapshot -H -o name <snap>'(존재-판정) + 'get -H -o value written <snap>'(property-read, R1) 로 실제 호출됨 (셸 미경유 argv)"
else
    fail "S5: mock zfs 호출 증거 부족 (핸들러가 zfs list/get 을 spawn 하지 않았을 수 있음)"
    echo "---- zfs_mock.log ----"; cat "$MOCK_LOG" 2>/dev/null || echo "(로그 없음)"
fi

# ═══════════════════════════════════════════════════════════════
# 단계 6 (I-1 반사실 — 감사 정확성): snapshot param 누락 → -32602 응답이고
#   감사 DB 에 result='fail' 로 기록돼야 한다. 옛 버그는 이 메서드가
#   async-result 인데 g_async_methods 미등록이라, 디스패처가 dispatch 시점에
#   **무조건 'ok'**(에러 응답까지 'ok')를 남겼다 — 그때는 fail 행이 0이라 이
#   단언이 RED. 수정: async 등록 + 완료콜백/조기검증 경로에서 실결과 audit.
#   성공케이스(S2~S4 유효 호출)는 완료콜백이 'ok' 를 남긴다.
# ═══════════════════════════════════════════════════════════════
if command -v sqlite3 >/dev/null 2>&1; then
    RESP_NOPARAM="$(send_rpc '{"jsonrpc":"2.0","method":"backup.snapshot.verify","params":{},"id":"vn"}')"
    info "S6: no-param 응답 = ${RESP_NOPARAM}"
    # 성공케이스 audit 는 async 완료콜백에서 기록되므로 잠깐 폴링(에러케이스는 동기).
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
