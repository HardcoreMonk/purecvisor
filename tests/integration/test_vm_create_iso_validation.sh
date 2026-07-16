#!/usr/bin/env bash
# tests/integration/test_vm_create_iso_validation.sh
#
# CMP-3: 라이브 vm.create 파라미터 검증 배선 결정적 E2E (실/격리 데몬)
# ---------------------------------------------------------------------------
# 검증 대상: dispatcher.c 의 라이브 handle_vm_create(static)가 iso_path 를
# pcv_validate_vm_create_params 로 실검증한다(PCV_SAFETY_CONTROL: vm-create-iso-validation).
# 수정 전에는 라이브 핸들러가 검증기를 호출하지 않아(검증은 handler_vm_lifecycle.c 의
# 무호출 dead 본에만 존재) iso_path=/etc/shadow 같은 임의 호스트 파일을 CD-ROM 으로
# 마운트할 수 있었다. 배선 후에는 확장자(.iso/.img)·절대경로·no-".." 규칙 위반 시
# op-lock/create_vm_async(부작용) 이전에 -32602 로 거부되어야 한다.
#
# 라이브 배선 단언(핵심): iso_path=/etc/shadow → 응답 .error != null AND
#   NOT .result.accepted == true. 미배선 트리에서는 accepted=true 로 통과 → RED.
#
# 결정적 재현(실 VM/실 libvirt/실 zfs 불요):
#   라이브 핸들러의 사전검사(libvirt lookup + zfs list + 파일존재)는 격리 환경에서
#   대상 부재로 exists=FALSE 로 degrade → 제어가 신규 검증 지점에 도달하고 거부한다.
#   거부는 op-lock/create 부작용 이전에 일어나므로 상태 변경이 전혀 없다.
#
# 케이스:
#   C1  iso_path=/etc/shadow                       (확장자 위반 — 임의파일 마운트)
#   C2  iso_path=/pcvpool/iso/../../etc/shadow      (경로순회 traversal)
#   C3  base_image=/etc/shadow                      (확장자 위반 — 임의파일 디스크 흡입, CMP-3 확장)
#   C4  base_image=/pcvpool/img/../../etc/shadow.qcow2 (경로순회 traversal)
#   모두 거부(.error != null, accepted 아님)여야 PASS.
#
# 격리 방식(test_vm_lock_cross_unlock.sh 관습 재사용):
#   - bubblewrap(bwrap) 사용자 네임스페이스 uid-0 맵핑, /var/lib/purecvisor 와
#     /etc/purecvisor 를 mktemp 임시 디렉터리로 bind → 프로덕션 DB/소켓 완전 shadow.
#   - libvirt 는 PCV_LIBVIRT_URI=test:///default (인메모리 mock)로 고정.
#   - UDS 직결은 ADMIN 로 취급되어 토큰 불요.
#
# 전제조건 부재 시(bwrap/python3/setsid/userns 불가) 깨끗이 SKIP(exit 0).
#
# 실행: bash tests/integration/test_vm_create_iso_validation.sh
# 부작용: 없음(모든 상태는 mktemp -d 임시 디렉터리, 종료 시 정리).

set -uo pipefail

GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS=0; FAIL=0
pass() { echo -e "${GREEN}[PASS]${NC} $*"; PASS=$((PASS+1)); }
fail() { echo -e "${RED}[FAIL]${NC} $*"; FAIL=$((FAIL+1)); }
info() { echo -e "${CYAN}[INFO]${NC} $*"; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
DAEMON_BIN="$REPO/bin/purecvisorsd"

skip() { echo -e "${YELLOW}[SKIP]${NC} CMP-3 E2E: $*"; exit 0; }

# ── 전제조건 검사 ────────────────────────────────────────────────
command -v bwrap    >/dev/null 2>&1 || skip "bwrap(bubblewrap) 미설치"
command -v python3  >/dev/null 2>&1 || skip "python3 미설치"
command -v setsid   >/dev/null 2>&1 || skip "setsid 미설치"
command -v jq       >/dev/null 2>&1 || skip "jq 미설치"

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
    for p in 28092 29092 31092 33092 37092 41092 43092; do
        if ! ss -ltn 2>/dev/null | grep -qE ":${p}([[:space:]]|$)"; then
            echo "$p"; return 0
        fi
    done
    return 1
}
PORT="$(pick_free_port)" || skip "빈 포트 확보 실패"

# ── 격리 상태 디렉터리 ────────────────────────────────────────────
STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-cmp3.XXXXXX")"
mkdir -p "$STATE/var-lib" "$STATE/etc" "$STATE/mockbin"
SOCK="$STATE/var-lib/daemon.sock"
VM="se-isocheck-001"

# ── mock zfs — 존재검사를 not-found 로 즉시 응답 ───────────────────
# 라이브 handle_vm_create 의 사전 존재검사는 zfs list <pool>/<vm> 를 spawn 한다.
# bwrap 격리(no /dev/zfs) 안에서 실제 zfs list 는 10s 타임아웃까지 블록되어 검증
# 지점 도달이 느려진다. pcv_spawn 이 강제하는 PATH 최우선(/usr/sbin)에 mock 을
# bind → list 를 exit 1(dataset 부재)로 즉답시켜 exists=FALSE 로 degrade 하게 한다.
# (검증 자체는 mock 무관 — mock 은 도달 속도만 확정한다.)
cat > "$STATE/mockbin/zfs" <<'MOCKEOF'
#!/bin/sh
# mock zfs (CMP-3 효과-테스트 전용). 컨테이너 /usr/sbin/zfs 로 bind 됨.
case "$1" in
    list) exit 1 ;;   # dataset not found → 존재검사 exists=FALSE
    *)    exit 0 ;;
esac
MOCKEOF
chmod +x "$STATE/mockbin/zfs"

# ── UDS JSON-RPC 클라이언트 (응답 후 서버가 소켓 close → recv EOF) ──
cat > "$STATE/uds_call.py" <<'PYEOF'
import socket, sys
sock_path, payload = sys.argv[1], sys.argv[2]
try:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(15.0)
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
admin_password = Cmp3E2eFixedPw-not-for-prod
jwt_secret = cmp3-e2e-fixed-secret-not-for-prod-0001
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
        --ro-bind "$STATE/mockbin/zfs" /usr/sbin/zfs \
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

# vm.create 거부 단언: 응답에 .error 가 있고 .result.accepted 가 true 가 아니어야 한다.
assert_rejected() {  # $1 = label   $2 = iso_path
    local label="$1" iso="$2" resp
    resp="$(uds_call "{\"jsonrpc\":\"2.0\",\"method\":\"vm.create\",\"params\":{\"name\":\"$VM\",\"vcpu\":1,\"memory_mb\":512,\"disk_size_gb\":5,\"iso_path\":\"$iso\"},\"id\":\"$label\"}")"
    info "$label: iso_path='$iso' 응답 = ${resp:-<empty>}"
    if [ -z "${resp:-}" ]; then
        fail "$label: 빈 응답 (데몬 무응답)"
        return
    fi
    local has_error accepted
    has_error="$(printf '%s' "$resp" | jq -r 'if (.error != null) then "yes" else "no" end' 2>/dev/null)"
    accepted="$(printf '%s' "$resp" | jq -r 'if (.result.accepted == true) then "yes" else "no" end' 2>/dev/null)"
    if [ "$has_error" = "yes" ] && [ "$accepted" != "yes" ]; then
        pass "$label: iso_path='$iso' 거부됨 (.error != null, accepted 아님) — 라이브 배선 실검증"
    else
        fail "$label: iso_path='$iso' 거부 안 됨 (has_error=$has_error accepted=$accepted) — 검증 미배선/회귀"
    fi
}

# base_image 거부 단언 (CMP-3 확장): base_image는 qemu-img convert 입력으로 host FS에서
# 직접 읽혀 VM 디스크로 기록되므로 iso_path와 동일 신뢰경계. 미검증 시 임의파일 흡입·순회.
assert_rejected_base_image() {  # $1 = label   $2 = base_image
    local label="$1" bimg="$2" resp
    resp="$(uds_call "{\"jsonrpc\":\"2.0\",\"method\":\"vm.create\",\"params\":{\"name\":\"$VM\",\"vcpu\":1,\"memory_mb\":512,\"disk_size_gb\":5,\"base_image\":\"$bimg\"},\"id\":\"$label\"}")"
    info "$label: base_image='$bimg' 응답 = ${resp:-<empty>}"
    if [ -z "${resp:-}" ]; then
        fail "$label: 빈 응답 (데몬 무응답)"
        return
    fi
    local has_error accepted
    has_error="$(printf '%s' "$resp" | jq -r 'if (.error != null) then "yes" else "no" end' 2>/dev/null)"
    accepted="$(printf '%s' "$resp" | jq -r 'if (.result.accepted == true) then "yes" else "no" end' 2>/dev/null)"
    if [ "$has_error" = "yes" ] && [ "$accepted" != "yes" ]; then
        pass "$label: base_image='$bimg' 거부됨 (.error != null, accepted 아님) — base_image 실검증"
    else
        fail "$label: base_image='$bimg' 거부 안 됨 (has_error=$has_error accepted=$accepted) — 검증 미배선/회귀"
    fi
}

echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  CMP-3 라이브 vm.create ISO 검증 배선 결정적 E2E     ${NC}"
echo -e "${CYAN}  binary=$DAEMON_BIN${NC}"
echo -e "${CYAN}  state=$STATE  uds=$SOCK${NC}"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"

# ═══════════════════════════════════════════════════════════════
# 단계 1: 격리 데몬 기동
# ═══════════════════════════════════════════════════════════════
write_conf
if ! boot; then
    fail "S1: 격리 데몬이 UDS 응답 실패"
    echo "---- daemon.log (마지막 30줄) ----"; tail -30 "$STATE/daemon.log" 2>/dev/null
    echo -e "\n${RED}데몬 기동 실패로 중단${NC}"; exit 1
fi
pass "S1: 격리 데몬 기동 + UDS 응답 ($SOCK)"

# ═══════════════════════════════════════════════════════════════
# 단계 2 (CMP-3 핵심): iso_path=/etc/shadow → 거부
#   확장자(.iso/.img) 위반 → 임의 호스트 파일 CD-ROM 마운트 차단.
# ═══════════════════════════════════════════════════════════════
assert_rejected "C1-etc-shadow" "/etc/shadow"

# ═══════════════════════════════════════════════════════════════
# 단계 3: iso_path=/pcvpool/iso/../../etc/shadow → 거부 (경로순회)
# ═══════════════════════════════════════════════════════════════
assert_rejected "C2-traversal" "/pcvpool/iso/../../etc/shadow"

# ═══════════════════════════════════════════════════════════════
# 단계 4 (CMP-3 확장): base_image 검증 — 임의 호스트 파일 흡입/경로순회 차단
#   C3  base_image=/etc/shadow                        (확장자 위반 — 임의파일→디스크 흡입)
#   C4  base_image=/pcvpool/img/../../etc/shadow.qcow2 (경로순회)
# ═══════════════════════════════════════════════════════════════
assert_rejected_base_image "C3-baseimg-shadow"   "/etc/shadow"
assert_rejected_base_image "C4-baseimg-traversal" "/pcvpool/img/../../etc/shadow.qcow2"

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
