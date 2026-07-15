#!/usr/bin/env bash
# tests/integration/test_backup_retention_effect.sh
#
# ADR-0025(반사실 검증) 효과-테스트 — STO-2/AF-S4 스냅샷 리텐션 prune 데이터유실 방지
# ---------------------------------------------------------------------------
# 이 테스트가 지키는 것: 리텐션 prune(`_prune_snapshots_by_prefix`)이 **시스템 예약
# 네임스페이스(pcv-incr-)** 스냅샷만 zfs destroy 하고, 사용자가 우연히 만든 옛
# 접두(incr-golden 등) 스냅샷은 절대 파괴하지 않는다. 또한 사용자 vm.snapshot.create
# 가 예약 접두 "pcv-" 로 시작하는 이름을 거부한다.
#
# 버그(수정 전): prune 이 이름 접두 "incr-" 만 보고 destroy 했으므로, 사용자가
# vm.snapshot.create 로 만든 "incr-golden"(또는 "s3-*")이 리텐션 초과 시 우연히
# 파괴됐다(무성 데이터유실). 시정 = 시스템 스냅샷을 pcv- 예약 네임스페이스로 이동
# (pcv-incr-/pcv-s3-, 기존 pcv-auto- 와 통일) + 사용자 create 에서 pcv- 접두 거부.
#
# 반사실(counterfactual):
#   - (a) 사용자 생존: INCR_SNAP_PREFIX rename(incr- → pcv-incr-)을 되돌리면
#         prune 필터가 다시 "incr-" 가 되어 "@incr-golden" 을 destroy → 이 단언 RED.
#   - (c) create 예약: reservation 블록을 제거하면 "pcv-golden" create 가 성공
#         (-32602 아님, audit fail 없음) → 이 단언 RED("보고성공 무동작" 클래스 차단).
#
# 드라이버(prune 도달 경로): backup.incremental({"name": vm}) — accepted+async 프로덕션
#   경로 (STO-5). accepted 응답을 먼저 받고, GTask 워커가
#   _prune_snapshots_by_prefix(vm, INCR_SNAP_PREFIX, incr_retention_count)에
#   도달한다. daemon.conf 에서 [backup] incr_retention_count=1 로 설정해 초과분을
#   강제 prune 시킨다. 외부 의존은 로컬뿐(zfs list/snapshot/send/destroy) — s3 처럼
#   ssh peer 의존이 없어 mock zfs 만으로 완결된다.
#   ⚠ async 이므로 destroy 는 워커 스레드에서 진행 — S2 poll 은 non-empty 가 아니라
#     완전 destroy 셋(마지막 예상 @pcv-incr-B)까지 대기해야 TOCTOU 가 없다.
#
#   pcv_backup_incremental 이 부르는 외부 명령(코드 확인):
#     1) _list_all_snapshots       → zfs list -H -o name -s creation -t snapshot -r <ds>
#     2) zfs snapshot <ds>@pcv-incr-<ts>            (새 증분 스냅샷)
#     3) /bin/sh -c "zfs send -i <prev> <new> > <f>"(prev 존재 시 증분 스트림; 파일 저장)
#     4) _prune_snapshots_by_prefix → zfs list ...(2회차) → 초과분 zfs destroy
#   따라서 mock zfs 는 list/snapshot/send/destroy 만 처리하면 되고, destroy 대상을
#   로그 파일에 남겨 prune 이 정확히 무엇을 지웠는지 host 에서 관측한다.
#
# 왜 mock zfs 인가 (verify 효과-테스트와 동일 근거):
#   pcv_spawn_sync 는 자식 PATH 를 /usr/sbin:/usr/bin:/sbin:/bin 으로 강제한다.
#   따라서 mock `zfs` 를 강제 PATH 최우선 /usr/sbin/zfs 에 bind-mount 로 덮는다.
#   /bin/sh -c "zfs send ..." 의 zfs 도 이 강제 PATH 로 mock 을 해소한다.
#
# 격리 방식(verify/SEC-2/DISP-1 harness 관습 재사용):
#   - bubblewrap(bwrap) 사용자 네임스페이스 uid-0, /var/lib/purecvisor·/etc/purecvisor
#     를 mktemp 임시 디렉터리로 bind → 프로덕션 DB/소켓 완전 shadow.
#   - libvirt 는 test:///default (인메모리 mock). backup.incremental 은 libvirt 미접촉.
#   - UDS 소켓 격리 디렉터리 내부 생성 → host 에서 nc -U 직결(로컬 신뢰 경로, 인증 불요).
#   - audit DB 는 S6 반사실용(create 예약 거부가 fail 로 기록되는지) sqlite3 로 검사.
#
# 전제조건 부재 시(bwrap/nc/setsid/userns/sqlite3 불가) 깨끗이 SKIP(exit 0).
# 실행: bash tests/integration/test_backup_retention_effect.sh
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
DAEMON_BIN="${PCV_DAEMON_BIN:-$REPO/bin/purecvisorsd}"

skip() { echo -e "${YELLOW}[SKIP]${NC} backup-retention 효과-테스트: $*"; exit 0; }

# ── 전제조건 검사 ────────────────────────────────────────────────
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

# ── 고정 픽스처 ──────────────────────────────────────────────────
VM='pcvtest'
POOL='pcvpool/vms'          # daemon.conf [storage] zvol_pool 기본값과 일치
DS="$POOL/$VM"              # zfs list -r 대상 데이터셋

# ── 격리 상태 디렉터리 ────────────────────────────────────────────
STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-bkretention.XXXXXX")"
mkdir -p "$STATE/var-lib" "$STATE/etc" "$STATE/mockbin"
SOCK="$STATE/var-lib/daemon.sock"
MOCK_LOG="$STATE/var-lib/zfs_mock.log"        # 모든 zfs 호출 argv 증거
DESTROY_LOG="$STATE/var-lib/zfs_destroy.log"  # prune 이 destroy 한 대상(핵심 증거)
AUDIT_DB="$STATE/var-lib/pcv_audit.db"        # create 예약 거부 audit(S6 반사실)

# ── mock zfs — list/snapshot/send/destroy 처리 ───────────────────
# list  : creation 오름차순(오래된→최신) 6 픽스처 출력. dataset 은 마지막 인자에서 취함.
#         [incr-golden(사용자), incr-legacy-1, incr-legacy-2(레거시 시스템),
#          pcv-incr-A, pcv-incr-B, pcv-incr-C(신규 시스템)]
# snapshot: exit 0 (새 증분 스냅샷 생성 성공)
# send   : exit 0, harmless 바이트 emit (sh 리다이렉트로 파일에 기록됨)
# destroy: exit 0 + destroy 대상(마지막 인자)을 DESTROY_LOG 에 append
cat > "$STATE/mockbin/zfs" <<'MOCKEOF'
#!/bin/sh
# mock zfs (효과-테스트 전용). 컨테이너 /usr/sbin/zfs 로 bind 됨.
printf 'MOCKZFS %s\n' "$*" >> /var/lib/purecvisor/zfs_mock.log 2>/dev/null || true
last=""
for a in "$@"; do last="$a"; done   # dataset(list) / snapshot 이름은 마지막 인자
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
    printf 'MOCKSTREAM\n'    # sh 의 '> file' 로 리다이렉트되어 백업 파일 생성
    exit 0 ;;
  destroy)
    printf 'DESTROY %s\n' "$last" >> /var/lib/purecvisor/zfs_destroy.log 2>/dev/null || true
    exit 0 ;;
  *)
    exit 0 ;;
esac
MOCKEOF
chmod +x "$STATE/mockbin/zfs"

# ── 데몬 설정 (UDS 소켓 격리 + retention 1 강제) ──────────────────
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

# ── UDS JSON-RPC 헬퍼 ────────────────────────────────────────────
send_rpc() { echo "$1" | nc -U "$SOCK" 2>/dev/null || true; }

# ── 데몬 라이프사이클 ─────────────────────────────────────────────
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

# ═══════════════════════════════════════════════════════════════
# 단계 1: 격리 데몬 기동 + UDS 소켓 서빙 확인
# ═══════════════════════════════════════════════════════════════
if ! boot; then
    fail "S1: 격리 데몬이 UDS 소켓 프로브에 응답 실패"
    echo "---- daemon.log (마지막 30줄) ----"; tail -30 "$STATE/daemon.log" 2>/dev/null
    note "로컬에서 데몬 부팅이 불가하면 이 테스트는 .50 E2E 서버 대상이다."
    echo -e "\n${RED}데몬 기동 실패로 중단${NC}"; exit 1
fi
pass "S1: 격리 데몬 기동 + UDS 소켓($SOCK) 프로브 응답 (서빙 확인)"

# ═══════════════════════════════════════════════════════════════
# 단계 2: prune 드라이버 — backup.incremental 실행
#   incr_retention_count=1 → pcv-incr- 집합 [A,B,C] 중 오래된 A,B destroy, C 보존.
# ═══════════════════════════════════════════════════════════════
RESP_INCR="$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"backup.incremental\",\"params\":{\"name\":\"${VM}\"},\"id\":\"bi\"}")"
info "S2: backup.incremental 응답 = ${RESP_INCR}"
# STO-5: async 워커가 prune → destroy 한다. non-empty poll 은 첫 destroy(@pcv-incr-A)
# 에서 깨져 @pcv-incr-B 미기록 상태를 관측하는 TOCTOU 가 있다. 마지막 예상 destroy
# (@pcv-incr-B)가 로그에 나타날 때까지 대기해 완전 destroy 셋을 보장한다.
for _i in $(seq 1 40); do
    [ -f "$DESTROY_LOG" ] && grep -Fq '@pcv-incr-B' "$DESTROY_LOG" && break
    sleep 0.25
done
info "S2: zfs destroy 로그 =\n$(cat "$DESTROY_LOG" 2>/dev/null || echo '(없음)')"

# ── 단언 (STO-5 반사실 ①): accepted 응답 shape ──
# async 오프로드면 즉시 {"status":"accepted"} 회신. 동기 복원 시 {snapshot,...}
# 이므로 이 단언 RED = STO-5 반사실.
if echo "$RESP_INCR" | grep -q '"status"[[:space:]]*:[[:space:]]*"accepted"'; then
    pass "S2(STO-5): backup.incremental → accepted 응답 (blocking zfs send 를 워커로 오프로드)"
else
    fail "S2(STO-5): accepted 응답 아님 (resp='$RESP_INCR') — 동기 핸들러 회귀 의심(데몬 블록)"
fi

# ── 단언 (STO-5 반사실 ②): 완료 콜백이 실결과 audit ──
# async 메서드는 디스패처가 audit 하지 않으므로(ADR-0018) 워커 audit 이 유일 기록(I-1).
# 동기 복원 시 워커 audit 이 사라져 result='ok' row 가 없으므로 이 단언 RED.
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

# ── 단언 (a): 사용자 스냅샷 생존 (destroy 로그에 @incr-golden 없음) ──
if [ -f "$DESTROY_LOG" ] && grep -Fq '@incr-golden' "$DESTROY_LOG"; then
    fail "S2a(반사실): 사용자 '@incr-golden' 이 destroy됨 — prune 이 pcv-incr- 로 필터하지 않음(rename 회귀=데이터유실)"
else
    pass "S2a: 사용자 '@incr-golden' 생존 (prune 필터가 pcv-incr- 네임스페이스로 한정됨)"
fi
# 레거시 시스템 스냅샷도 자동 prune 되지 않아야 한다(설계: 레거시 접두 자동 prune 금지).
if [ -f "$DESTROY_LOG" ] && grep -Eq '@incr-legacy-[12]' "$DESTROY_LOG"; then
    fail "S2a': 레거시 '@incr-legacy-*' 가 destroy됨 — 레거시 접두 자동 prune 금지 위반"
else
    pass "S2a': 레거시 '@incr-legacy-*' 미파괴 (레거시 접두 자동 prune 금지 — 운영 후속으로 유예)"
fi

# ── 단언 (b): 시스템 스냅샷 오래된 것부터 prune (A→B destroy, C 보존) ──
has_A=0; has_B=0; has_C=0
[ -f "$DESTROY_LOG" ] && grep -Fq '@pcv-incr-A' "$DESTROY_LOG" && has_A=1
[ -f "$DESTROY_LOG" ] && grep -Fq '@pcv-incr-B' "$DESTROY_LOG" && has_B=1
[ -f "$DESTROY_LOG" ] && grep -Fq '@pcv-incr-C' "$DESTROY_LOG" && has_C=1
# 순서: A 가 B 보다 먼저 destroy (오래된 것부터)
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

# ── mock zfs 가 실제 spawn 됐다는 증거(우연 일치 아님) ──
if [ -f "$MOCK_LOG" ] \
   && grep -Fq "list -H -o name -s creation -t snapshot -r ${DS}" "$MOCK_LOG" \
   && grep -Fq "snapshot ${DS}@pcv-incr-" "$MOCK_LOG"; then
    pass "S2c: mock zfs 가 list(creation 정렬) + snapshot(pcv-incr- 접두)로 실제 호출됨 (셸 미경유 argv)"
else
    fail "S2c: mock zfs 호출 증거 부족 (backup.incremental 이 zfs list/snapshot 을 spawn 하지 않았을 수 있음)"
    echo "---- zfs_mock.log ----"; cat "$MOCK_LOG" 2>/dev/null || echo "(로그 없음)"
fi

# ═══════════════════════════════════════════════════════════════
# 단계 3 (핵심 반사실 c): 사용자 create 예약 거부
#   vm.snapshot.create {"snapshot_name":"pcv-golden"} → -32602 &
#   audit DB 에 vm.snapshot.create result='fail'. 예약 블록 제거하면 성공(RED).
#   예약 검사가 _zfs_dataset_exists 앞이므로 zfs 상호작용 없이 성립한다.
# ═══════════════════════════════════════════════════════════════
RESP_CREATE="$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"vm.snapshot.create\",\"params\":{\"name\":\"${VM}\",\"snapshot_name\":\"pcv-golden\"},\"id\":\"sc\"}")"
info "S3: pcv-golden create 응답 = ${RESP_CREATE}"
# audit 는 비동기 큐 → 라이터 스레드 → DB 이므로 잠깐 폴링(WAL).
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

# 대조군: pcv- 접두가 아닌 정상 이름은 예약에 걸리지 않아야 한다(과잉 거부 아님 확인).
# (실제 zfs snapshot 은 mock 이 exit 0 처리 — create 자체 성공 여부는 여기서 단언하지 않음)
info "S3': (참고) 예약은 접두 'pcv-' 에만 적용 — 일반 이름은 통과 경로"

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
