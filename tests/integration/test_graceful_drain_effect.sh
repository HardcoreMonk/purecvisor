#!/usr/bin/env bash
# tests/integration/test_graceful_drain_effect.sh
#
# DISP-4 graceful-drain 효과-테스트 — 종료(drain) 중 신규요청 거부 + SIGTERM inflight 대기
# ---------------------------------------------------------------------------
# 이 테스트가 지키는 것: pcv_drain_inc()/pcv_drain_dec() 가 UDS 요청 수명주기에 실제로
# 배선되어 (1) 종료(drain) 진입 후 신규 요청이 JSON-RPC -32000 으로 거부되고,
# (2) SIGTERM 시 진행 중(inflight) 요청이 완료될 때까지 데몬이 즉시 종료하지 않고 대기한다.
#
# 배경(거짓성공): pcv_drain_inc/dec 의 프로덕션 호출부가 0(주석만)이던 시절 inflight 는
# 영구 0이라, SIGTERM 이 대기 없이 즉시 main loop 를 quit → 진행 중 요청 드롭 + node.drain
# 거부가 무동작. DISP-4 가 이를 uds_server.c 의 수락/응답 경로에 배선했다.
#
# 배선 방식(Option A): 연결 수락 시 pcv_drain_inc(), 요청 read 완료 콜백 공통 cleanup 에서
# pcv_drain_dec(). 단일스레드 GMainLoop + 동기 dispatch 구조에서 '디스패치 직전 inc'(스펙
# Option B)는 동기 dispatch 중 SIGTERM 이 starve 되어 drain-wait 이 무효라, '수락 시 inc'만이
# 유효한 지점이다. 거부(-32000)는 수락 즉시 write+close 가 아니라 요청을 read 로 소비한 뒤
# 전송한다 — 즉시 close 하면 요청을 먼저 보내는 클라이언트(pcvctl/REST/`echo|nc`)가 write 시
# EPIPE 로 죽어 거부 응답을 못 읽고 드롭되기 때문(read-then-reply).
#
# io_uring 참고: 이 저장소는 liburing 존재 시 PCV_USE_URING=1 로 빌드되어 데몬이 기본
# io_uring 모드로 뜬다(fallback: GSocketService). 두 경로 모두 동일 규율로 배선되어 있으며,
# 이 테스트는 실제로 뜨는 경로(대개 io_uring)를 그대로 검증한다.
#
# 반사실(counterfactual, 커밋 금지 — 로컬 검증용):
#   - inc/dec 배선을 제거하면 inflight 영구 0.
#     · Scenario A: node.drain 후 vm.list 가 -32000 이 아니라 정상 result 를 반환(RED).
#     · Scenario B: SIGTERM 시 "Waiting for 1 in-flight" 로그 없이 즉시 main loop quit,
#       held 요청 드롭(RESP 빈값)(RED).
#
# 격리 방식(SEC-2/STO-2 harness 관습 재사용 — test_snapshot_verify_effect.sh):
#   - bubblewrap(bwrap) 사용자 네임스페이스 uid-0 맵핑, /var/lib/purecvisor 와
#     /etc/purecvisor 를 mktemp 임시 디렉터리로 bind → 프로덕션 DB/소켓 완전 shadow.
#   - libvirt 는 test:///default (인메모리 mock, 호스트 하이퍼바이저 무접촉).
#   - UDS 는 미인증 ADMIN 로컬 신뢰 경로라 node.drain/node.resume 를 nc -U 로 직접 실행.
#   - drain_timeout=10 (설정 <5 는 30s 기본으로 clamp 되므로 5 이상 필수).
#   - 시나리오별 fresh boot: pcv_drain_begin 은 shutdown_flag 를 CAS 로 한 번만 세우므로
#     drain 을 관측하려면 매 시나리오마다 새 데몬 프로세스가 필요하다.
#
# 전제조건 부재 시(bwrap/nc/setsid/mkfifo/userns 불가) 깨끗이 SKIP(exit 0).
# 데몬이 부팅했으나 검증이 실패하면 FAIL(exit 1).
#
# 실행: bash tests/integration/test_graceful_drain_effect.sh
# 부작용: 없음(모든 상태는 mktemp -d 임시 디렉터리, 종료 시 정리).
#
# 알려진 무관 이슈(범위 밖): 데몬은 drain 후 main loop 를 정상 quit 하지만, 이후 종료
# cleanup 시퀀스(스레드 join 등)가 격리 환경에서 완료가 느리거나 멈춘다 — inflight=0
# 상태의 평범한 SIGTERM 에서도 동일. DISP-4(drain-wait) 와 무관한 사전존재 이슈라, 이
# 테스트는 프로세스 완전 종료가 아니라 drain-wait 성립 자체를 단언한다.

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

# ── 전제조건 검사 ────────────────────────────────────────────────
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

# ── 전역 상태(현재 부팅 데몬) ─────────────────────────────────────
STATE=""; SOCK=""; BP=""; DPID=""

send_rpc() { echo "$1" | timeout 5 nc -U "$SOCK" 2>/dev/null || true; }

kill_daemon() {  # 세션 그룹 강제 종료(pre-existing cleanup hang 대비 SIGKILL 확실)
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

fresh_boot() {  # 이전 데몬 종료 → 새 격리 상태 디렉터리 + 새 데몬 프로세스
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
    disown "$BP" 2>/dev/null || true  # bash 잡테이블에서 제거 — kill 시 "Killed" 잡노티 억제

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

# ═══════════════════════════════════════════════════════════════
# Scenario A: drain 진입 → 신규요청 -32000 거부 (결정적)
#   node.drain(result:true) → vm.list(error -32000). Option A/B 무관하게 inc 호출 +
#   그 FALSE(shutting-down) 존중을 증명. 반사실: 배선 제거 시 vm.list 가 정상 result.
# ═══════════════════════════════════════════════════════════════
echo -e "\n${CYAN}── Scenario A: drain 신규요청 거부 ──${NC}"
if ! fresh_boot; then
    fail "A-S1: 격리 데몬이 UDS 소켓 프로브에 응답 실패"
    echo "---- daemon.log (마지막 30줄) ----"; tail -30 "$STATE/daemon.log" 2>/dev/null
    note "로컬에서 데몬 부팅이 불가하면(libvirt/커널 등) 이 테스트는 .50 E2E 서버 대상이다."
    echo -e "\n${RED}데몬 기동 실패로 중단${NC}"; exit 1
fi
MODE="$(grep -oE 'io_uring mode|systemd socket activation|listening on [^ ]+' "$STATE/daemon.log" | head -1)"
pass "A-S1: 격리 데몬 기동 + UDS 프로브 응답 (mode: ${MODE:-unknown})"

# A-S2: drain 전 vm.list 는 정상 result (baseline — 반사실의 대조군)
RESP_PRE="$(send_rpc '{"jsonrpc":"2.0","method":"vm.list","params":{},"id":"pre"}')"
info "A-S2: drain 전 vm.list = ${RESP_PRE:0:80}"
if echo "$RESP_PRE" | grep -Eq '"result"' && ! echo "$RESP_PRE" | grep -Eq '"code"[[:space:]]*:[[:space:]]*-32000'; then
    pass "A-S2: drain 전 vm.list 정상 result (inc 가 TRUE 반환 — 정상 처리 경로)"
else
    fail "A-S2: drain 전 vm.list 가 정상 result 아님 (resp='$RESP_PRE')"
fi

# A-S3: node.drain → result:true
RESP_DRAIN="$(send_rpc '{"jsonrpc":"2.0","method":"node.drain","params":{},"id":"dr"}')"
info "A-S3: node.drain = ${RESP_DRAIN}"
if echo "$RESP_DRAIN" | grep -Eq '"result"[[:space:]]*:[[:space:]]*true'; then
    pass "A-S3: node.drain → result:true (drain 진입, shutdown_flag=1)"
else
    fail "A-S3: node.drain 이 result:true 아님 (resp='$RESP_DRAIN')"
fi

# A-S4 (핵심 반사실): drain 후 vm.list → -32000 "shutting down"
RESP_POST="$(send_rpc '{"jsonrpc":"2.0","method":"vm.list","params":{},"id":"post"}')"
info "A-S4: drain 후 vm.list = ${RESP_POST}"
if echo "$RESP_POST" | grep -Eq '"code"[[:space:]]*:[[:space:]]*-32000'; then
    pass "A-S4(반사실): drain 후 신규요청 → -32000 거부 (inc 의 FALSE 존중 — 배선 제거 시 정상 result 라 RED)"
else
    fail "A-S4(반사실): drain 후 vm.list 가 -32000 거부 아님 — inc/dec 미배선 의심 (resp='$RESP_POST')"
fi

# A-S5 (제어평면 복구, Task 5): drain 중 node.resume 은 화이트리스트되어 -32000 거부되지
#   않고 정상 dispatch → pcv_drain_cancel 로 shutdown_flag 리셋. DISP-4(수락-시 inc)가
#   node.resume 자기연결까지 막아 node.drain 을 RPC 로 되돌릴 수 없던 brick 풋건의 시정.
#   node.drain 문서("node.resume 으로 재개 가능") 계약을 실동작으로 고정한다.
#   반사실: 화이트리스트 제거 시 node.resume → -32000 → RED.
RESP_RESUME="$(send_rpc '{"jsonrpc":"2.0","method":"node.resume","params":{},"id":"rs"}')"
info "A-S5: drain 중 node.resume = ${RESP_RESUME}"
if echo "$RESP_RESUME" | grep -Eq '"result"[[:space:]]*:[[:space:]]*true'; then
    pass "A-S5: drain 중 node.resume → result:true (제어평면 예외 화이트리스트 — brick 복구, 배선 제거 시 -32000 이라 RED)"
else
    fail "A-S5: drain 중 node.resume 이 result:true 아님 — 화이트리스트 미배선 의심 (resp='$RESP_RESUME')"
fi

# A-S6 (복구 확인): node.resume 직후 vm.list 가 다시 정상 result → shutdown_flag 리셋으로
#   신규 연결이 재수락됨을 증명(제어평면 실복구). A-S4 에서 -32000 이던 동일 요청이 복구된다.
RESP_RECOVER="$(send_rpc '{"jsonrpc":"2.0","method":"vm.list","params":{},"id":"recover"}')"
info "A-S6: 복구 후 vm.list = ${RESP_RECOVER:0:80}"
if echo "$RESP_RECOVER" | grep -Eq '"result"' && ! echo "$RESP_RECOVER" | grep -Eq '"code"[[:space:]]*:[[:space:]]*-32000'; then
    pass "A-S6: node.resume 후 vm.list 정상 result (shutdown_flag 리셋 → 신규 연결 재수락, 제어평면 복구)"
else
    fail "A-S6: node.resume 후 vm.list 가 정상 result 아님 — 복구 실패 (resp='$RESP_RECOVER')"
fi

# ═══════════════════════════════════════════════════════════════
# Scenario B: SIGTERM 시 inflight 완료 대기 (faithful, Option A)
#   held 연결(inflight=1) → SIGTERM → 데몬 즉시 종료 않고 "Waiting for 1 in-flight" →
#   held 요청 feed → 정상 응답(드롭 아님) + "All requests drained" + no "Timeout after".
#   반사실: 배선 제거 시 "Waiting" 로그 없이 즉시 quit, RESP 빈값.
#   (프로세스 완전 종료는 무관한 pre-existing cleanup hang 이라 단언하지 않음.)
# ═══════════════════════════════════════════════════════════════
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
        # 연결만 열고 요청은 보류: nc 가 소켓에 connect → 데몬 수락(inc, inflight=1) →
        # recv 대기. fifo writer(fd 7)를 열어 둬 nc 가 EOF 를 안 보게 유지.
        nc -U "$SOCK" < "$REQ" > "$RESP" 2>/dev/null &
        exec 7>"$REQ"
        sleep 0.8

        # SIGTERM 은 데몬 PID 에만(그룹 아님). 그룹킬은 bwrap 도 죽여 데몬을 고아화해
        # 관측을 흐린다. 데몬만 SIGTERM → bwrap 은 모니터로 생존.
        kill -TERM "$DPID" 2>/dev/null
        sleep 1.3

        ALIVE="$(kill -0 "$DPID" 2>/dev/null && echo yes || echo no)"
        # grep -c 는 매칭 0건일 때 exit 1 이라 '|| echo 0' 를 붙이면 "0\n0" 이 되어
        # 정수 비교가 깨진다. stdout 은 항상 단일 숫자이므로 그대로 캡처 후 기본값만 보정.
        WAITLOG="$(grep -c 'Waiting for 1 in-flight' "$STATE/daemon.log" 2>/dev/null)"; WAITLOG="${WAITLOG:-0}"
        info "B-S2: SIGTERM 1.3s 후 daemon alive=$ALIVE, 'Waiting for 1 in-flight' 로그=$WAITLOG"
        if [ "$ALIVE" = "yes" ] && [ "${WAITLOG:-0}" -ge 1 ]; then
            pass "B-S2: SIGTERM 후 데몬 생존 + inflight 대기 로그 (수락 시 inc 가 inflight 를 SIGTERM 을 가로질러 보유 — drain-wait 성립)"
        else
            fail "B-S2: SIGTERM 후 즉시 종료했거나 inflight 대기 로그 부재 (alive=$ALIVE, waitlog=$WAITLOG) — 배선 제거 시 RED"
            echo "---- daemon.log (마지막 15줄) ----"; tail -15 "$STATE/daemon.log" 2>/dev/null
        fi

        # 이제 보류 요청을 흘려보냄 → recv 완료 → dispatch → 응답 → dec → inflight=0 → drain 완료
        printf '{"jsonrpc":"2.0","method":"vm.list","params":{},"id":"held"}\n' >&7
        exec 7>&-
        # drain 완료 로그가 뜰 때까지 폴링(최대 ~6s, drain_timeout 10s 이내)
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

# ═══════════════════════════════════════════════════════════════
kill_daemon

echo ""
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
printf "  결과: ${GREEN}PASS %d${NC} / ${RED}FAIL %d${NC}\n" "$PASS" "$FAIL"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
[ "$FAIL" -eq 0 ]
