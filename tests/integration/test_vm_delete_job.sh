#!/usr/bin/env bash

set -uo pipefail

GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS=0; FAIL=0
pass() { echo -e "${GREEN}[PASS]${NC} $*"; PASS=$((PASS+1)); }
fail() { echo -e "${RED}[FAIL]${NC} $*"; FAIL=$((FAIL+1)); }
info() { echo -e "${CYAN}[INFO]${NC} $*"; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
DAEMON_BIN="$REPO/bin/purecvisorsd"

. "$SCRIPT_DIR/lib/pcv_isolated_daemon.sh"

skip() { echo -e "${YELLOW}[SKIP]${NC} vm.delete Job E2E: $*"; exit 0; }

command -v bwrap    >/dev/null 2>&1 || skip "bwrap(bubblewrap) 미설치"
command -v python3  >/dev/null 2>&1 || skip "python3 미설치"
command -v setsid   >/dev/null 2>&1 || skip "setsid 미설치"
command -v jq       >/dev/null 2>&1 || skip "jq 미설치"
command -v sqlite3  >/dev/null 2>&1 || skip "sqlite3 미설치"
pcv_iso_capable || skip "$PCV_ISO_SKIP_REASON"
if [ ! -x "$DAEMON_BIN" ]; then
    info "데몬 바이너리 없음 — make daemon 시도"
    make -C "$REPO" daemon >/dev/null 2>&1 || true
fi
[ -x "$DAEMON_BIN" ] || skip "데몬 바이너리 빌드 실패 ($DAEMON_BIN)"
pcv_iso_check_bin "$DAEMON_BIN" || skip "$PCV_ISO_SKIP_REASON"

pick_free_port() {
    local p
    for p in "$@"; do
        if ! ss -ltn 2>/dev/null | grep -qE ":${p}([[:space:]]|$)"; then
            echo "$p"; return 0
        fi
    done
    return 1
}
PORT_A="$(pick_free_port 28146 29146 31146 33146)" || skip "빈 포트 확보 실패"
PORT_B="$(pick_free_port 37146 41146 43146 44146)" || skip "빈 포트 확보 실패"

STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-z46.XXXXXX")"
pcv_iso_check_state_fs "$STATE" || { rm -rf "$STATE"; exit 1; }
mkdir -p "$STATE/mockbin"
for d in a b; do mkdir -p "$STATE/$d/var-lib" "$STATE/$d/etc"; done

cat > "$STATE/mockbin/zfs" <<'MOCKEOF'
#!/bin/sh

case "$1" in
    list) exit 1 ;;
    *)    exit 0 ;;
esac
MOCKEOF
chmod +x "$STATE/mockbin/zfs"

kill_daemon() {
    local f="$STATE/$1/bwrap.pid" bp i
    [ -f "$f" ] || return 0
    bp="$(cat "$f" 2>/dev/null || true)"
    [ -n "${bp:-}" ] || return 0
    kill -- "-$bp" 2>/dev/null || true
    kill "$bp" 2>/dev/null || true
    for i in $(seq 1 12); do
        kill -0 "$bp" 2>/dev/null || break
        sleep 0.25
    done
    kill -9 -- "-$bp" 2>/dev/null || true
    kill -9 "$bp" 2>/dev/null || true
    rm -f "$f"
}

cleanup() {
    kill_daemon a
    kill_daemon b
    rm -rf "$STATE" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

SOCK=""
uds_call() {
    printf '%s' "$1" | pcv_iso_uds_nc "$SOCK" 15 2>/dev/null
}
rpc() {
    uds_call "{\"jsonrpc\":\"2.0\",\"method\":\"$1\",\"params\":$2,\"id\":\"t\"}"
}

boot() {
    local w="$1" port="$2"; shift 2
    cat > "$STATE/$w/etc/daemon.conf" <<CONF
[daemon]
socket_path = /var/lib/purecvisor/daemon.sock
rest_port = $port
admin_user = admin
admin_password = Z46E2eFixedPw-not-for-prod
jwt_secret = z46-e2e-fixed-secret-not-for-prod-0001
libvirt_uri = test:///default
log_level = info
drain_timeout = 1
CONF
    SOCK="$STATE/$w/var-lib/daemon.sock"
    setsid "$PCV_ISO_BWRAP" \
        "${PCV_ISO_BWRAP_ARGS[@]}" \
        --bind "$STATE/$w/var-lib" /var/lib/purecvisor \
        --bind "$STATE/$w/etc" /etc/purecvisor \
        --ro-bind "$STATE/mockbin/zfs" /usr/sbin/zfs \
        --setenv PCV_LIBVIRT_URI test:///default \
        "$@" \
        --chdir / \
        "$DAEMON_BIN" > "$STATE/$w/daemon.log" 2>&1 < /dev/null &
    echo "$!" > "$STATE/$w/bwrap.pid"
    local i resp
    for i in $(seq 1 60); do
        if [ -S "$SOCK" ]; then
            resp="$(rpc vm.list '{}')"
            case "$resp" in
                *'"jsonrpc"'*|*'"result"'*|*'"error"'*) return 0 ;;
            esac
        fi
        sleep 0.5
    done
    return 1
}

check() {
    local label="$1" resp="$2" expr="$3"
    if [ -n "$resp" ] && [ "$(printf '%s' "$resp" | jq -r "if ($expr) then \"yes\" else \"no\" end" 2>/dev/null)" = "yes" ]; then
        pass "$label"
    else
        fail "$label — 응답=${resp:-<empty>}"
    fi
}

poll_status() {
    local r i
    for i in $(seq 1 50); do
        r="$(rpc vm.delete.status "{\"name\":\"$1\"}")"
        [ "$(printf '%s' "$r" | jq -r '.result.status // ""' 2>/dev/null)" = "completed" ] && break
        sleep 0.2
    done
    printf '%s' "$r"
}

echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  vm.delete Job 접수·완료·상태 조회 격리 E2E     ${NC}"
echo -e "${CYAN}  binary=$DAEMON_BIN${NC}"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"

if ! boot a "$PORT_A"; then
    fail "S1: 데몬 A 기동 실패"
    tail -30 "$STATE/a/daemon.log" 2>/dev/null
    exit 1
fi
pass "S1: 격리 데몬 A 기동 + UDS 응답"

R1="$(rpc vm.delete '{"name":"pcv-ghost"}')"
info "1: $R1"
check "1: vm.delete 접수 응답이 객체(accepted·name·job_id·status)" "$R1" \
  '.result.accepted == true and .result.name == "pcv-ghost" and (.result.job_id|type=="string" and startswith("job-")) and .result.status == "accepted"'
JOB1="$(printf '%s' "$R1" | jq -r '.result.job_id // ""' 2>/dev/null)"

S1="$(poll_status pcv-ghost)"
info "1b: $S1"
check "1b: vm.delete.status → completed·already_absent·같은 job_id·outcome_known" "$S1" \
  ".result.status == \"completed\" and .result.already_absent == true and .result.job_id == \"$JOB1\" and .result.outcome_known == true"
G1="$(rpc jobs.get "{\"job_id\":\"$JOB1\"}")"
check "1b: jobs.get → completed·target==pcv-ghost" "$G1" \
  '.result.status == "completed" and .result.target == "pcv-ghost"'

R2="$(rpc vm.delete '{"name":"6695eb01-f6a4-8304-79aa-97f2502e193f"}')"
info "2: $R2"
check "2: UUID 요청 → result name == test" "$R2" '.result.name == "test"'
S2="$(poll_status test)"
info "2: $S2"
check "2: vm.delete.status test → completed, already_absent 없음" "$S2" \
  '.result.status == "completed" and (.result.already_absent != true)'
AOK="$(sqlite3 -cmd ".timeout 5000" "$STATE/a/var-lib/pcv_audit.db" \
  "SELECT COUNT(*) FROM audit_log WHERE method='vm.delete' AND target='test' AND result='ok';" 2>/dev/null || echo "?")"
[ "$AOK" = "1" ] && pass "2: 감사 vm.delete/test/ok 정확히 1줄" || fail "2: 감사 ok 줄 수=$AOK (기대 1)"

check "3: vm.delete.status {} → -32602" "$(rpc vm.delete.status '{}')" '.error.code == -32602'
check "3: vm.delete.status bad name! → -32602" "$(rpc vm.delete.status '{"name":"bad name!"}')" '.error.code == -32602'

check "4: vm.delete.status never-deleted → not_found" "$(rpc vm.delete.status '{"name":"never-deleted"}')" \
  '.result == {"name":"never-deleted","status":"not_found"}'

R7="$(rpc vm.delete '{"name":"bad name!"}')"
check "7: vm.delete bad name! → -32602 기존 메시지" "$R7" \
  '.error.code == -32602 and (.error.message|startswith("Missing or invalid param: vm_id"))'

kill_daemon a

if ! boot b "$PORT_B" --setenv PCV_JOBS_DB_PATH /nonexistent-z46/jobs.db; then
    fail "S2: 데몬 B 기동 실패"
    tail -30 "$STATE/b/daemon.log" 2>/dev/null
    exit 1
fi
pass "S2: 격리 데몬 B(장부 비활성) 기동 + UDS 응답"

R5="$(rpc vm.delete '{"name":"pcv-ghost2"}')"
info "5: $R5"
check "5: 장부 비활성 vm.delete → -32603 Job persistence unavailable" "$R5" \
  '.error.code == -32603 and .error.message == "Job persistence unavailable — VM deletion rejected"'
LOCKS="$(sqlite3 -cmd ".timeout 5000" "$STATE/b/var-lib/vm_state.db" "SELECT COUNT(*) FROM vm_locks;" 2>/dev/null || echo "?")"
[ "$LOCKS" = "0" ] && pass "5: vm_locks 0줄 (잠금이 남지 않음)" || fail "5: vm_locks=$LOCKS (기대 0)"
check "5: 같은 요청 재전송도 -32603 (CONFLICT 아님)" "$(rpc vm.delete '{"name":"pcv-ghost2"}')" '.error.code == -32603'

check "6: 장부 비활성 vm.delete.status → -32603 Job ledger unavailable" "$(rpc vm.delete.status '{"name":"x1"}')" \
  '.error.code == -32603 and .error.message == "Job ledger unavailable"'

kill_daemon b

echo ""
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
printf "  결과: ${GREEN}PASS %d${NC} / ${RED}FAIL %d${NC}\n" "$PASS" "$FAIL"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
[ "$FAIL" -eq 0 ]
