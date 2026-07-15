#!/usr/bin/env bash
# tests/integration/test_sec2_bootstrap_fallback.sh
#
# SEC-2: 부트스트랩 fallback 백도어 차단 E2E (실/격리 데몬)
# ---------------------------------------------------------------------------
# 검증 대상: rest_server.c 로그인 fallback 이 (cfg_user, cfg_pass) 에 토큰을
# 발급하는 것은 "해당 사용자가 RBAC DB에 부재(진짜 첫 설치)"일 때뿐이다.
# _ensure_admin_user 가 부팅 시 admin 을 시딩하므로, 시딩 이후에는 daemon.conf
# 의 부트스트랩 자격증명이 fallback 으로 토큰을 얻지 못해야 한다(SEC-2).
#
# 격리 방식: 이 테스트는 프로덕션 데몬/DB/소켓을 절대 건드리지 않는다.
#   - bubblewrap(bwrap) 사용자 네임스페이스로 daemon 상태 경로
#     (/var/lib/purecvisor, /etc/purecvisor) 를 임시 디렉터리로 바인드 마운트.
#   - pcv_rbac_init() 은 /var/lib/purecvisor/rbac.db 를 하드코딩하므로,
#     바인드 마운트로만 안전하게 격리된다(설정/ENV 오버라이드 없음).
#   - 데몬은 비특권 userns 안에서 uid 0 으로 맵핑되어(호스트에서는 일반 사용자)
#     루트 요구 검사를 통과하되 호스트 권한은 얻지 않는다.
#   - REST 는 비기본 고포트에서 서빙(soup_server_listen_all 특성상 0.0.0.0
#     바인드 — 코드 변경 없이 localhost 한정 불가, 짧은 테스트 창으로 최소화).
#
# 전제조건 부재 시(bwrap/sqlite3/userns 불가) 깨끗이 SKIP(exit 0)한다.
#
# 실행: bash tests/integration/test_sec2_bootstrap_fallback.sh
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

skip() { echo -e "${YELLOW}[SKIP]${NC} SEC-2 E2E: $*"; exit 0; }

# ── 전제조건 검사 ────────────────────────────────────────────────
command -v bwrap    >/dev/null 2>&1 || skip "bwrap(bubblewrap) 미설치"
command -v sqlite3  >/dev/null 2>&1 || skip "sqlite3 미설치"
command -v curl     >/dev/null 2>&1 || skip "curl 미설치"
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

# ── 빈 포트 선택 ──────────────────────────────────────────────────
pick_free_port() {
    local p
    for p in 28080 29080 31080 33080 37080 41080 43080; do
        if ! ss -ltn 2>/dev/null | grep -qE ":${p}([[:space:]]|$)"; then
            echo "$p"; return 0
        fi
    done
    return 1
}
PORT="$(pick_free_port)" || skip "빈 포트 확보 실패"
BASE="http://127.0.0.1:$PORT/api/v1"

# ── 자격증명 (테스트 전용, 프로덕션과 무관) ───────────────────────
# P1 = admin 이 RBAC DB 에 실제로 보유한 비밀번호 → 로그인 허용
# P0 = daemon.conf 부트스트랩 비밀번호(cfg_pass) 이지만 DB 와 불일치 → 거부돼야 함
P1='AdminDbPass1x9Q'
P0='OldBootPass0z7K'

# ── 격리 상태 디렉터리 ────────────────────────────────────────────
STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-sec2.XXXXXX")"
mkdir -p "$STATE/var-lib" "$STATE/etc"

kill_daemon() {  # graceful then SIGKILL fallback (no pkill -f — 그러면 자기 명령줄까지 매칭)
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
    kill -9 -- "-$bp" 2>/dev/null || true   # 잔존 시 강제 종료
    kill -9 "$bp" 2>/dev/null || true
}

cleanup() {
    kill_daemon
    rm -f "$STATE/bwrap.pid"
    rm -rf "$STATE" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

write_conf() {  # $1 = admin_password
    cat > "$STATE/etc/daemon.conf" <<EOF
[daemon]
socket_path = /var/lib/purecvisor/daemon.sock
rest_port = $PORT
admin_user = admin
admin_password = $1
jwt_secret = sec2-e2e-fixed-secret-not-for-prod-0001
libvirt_uri = qemu:///system
log_level = info
drain_timeout = 1
EOF
}

boot() {  # 격리 데몬 기동 후 REST /health 200 대기
    setsid bwrap \
        --unshare-user --uid 0 --gid 0 \
        --ro-bind / / \
        --dev /dev \
        --proc /proc \
        --tmpfs /tmp \
        --bind "$STATE/var-lib" /var/lib/purecvisor \
        --bind "$STATE/etc" /etc/purecvisor \
        --chdir / \
        "$DAEMON_BIN" > "$STATE/daemon.log" 2>&1 < /dev/null &
    echo "$!" > "$STATE/bwrap.pid"
    local i c
    for i in $(seq 1 60); do
        c="$(curl -s -o /dev/null -w '%{http_code}' --max-time 2 "$BASE/health" 2>/dev/null || true)"
        [ "$c" = "200" ] && return 0
        sleep 0.5
    done
    return 1
}

stop() {
    kill_daemon
    for _ in $(seq 1 20); do
        [ "$(curl -s -o /dev/null -w '%{http_code}' --max-time 1 "$BASE/health" 2>/dev/null || echo 000)" = "000" ] && break
        sleep 0.3
    done
    rm -f "$STATE/bwrap.pid"
}

login_code() {  # $1=user $2=pass -> HTTP code
    curl -s -o /dev/null -w '%{http_code}' --max-time 6 -X POST "$BASE/auth/token" \
        -H 'Content-Type: application/json' \
        -d "{\"username\":\"$1\",\"password\":\"$2\"}" 2>/dev/null || echo 000
}
login_token() { # $1=user $2=pass -> access_token or empty
    curl -s --max-time 6 -X POST "$BASE/auth/token" \
        -H 'Content-Type: application/json' \
        -d "{\"username\":\"$1\",\"password\":\"$2\"}" 2>/dev/null \
      | python3 -c "import sys,json;print(json.load(sys.stdin).get('access_token',''))" 2>/dev/null || true
}

RBAC_DB="$STATE/var-lib/rbac.db"
AUDIT_DB="$STATE/var-lib/pcv_audit.db"

echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  SEC-2 부트스트랩 fallback 백도어 차단 E2E          ${NC}"
echo -e "${CYAN}  binary=$DAEMON_BIN${NC}"
echo -e "${CYAN}  port=$PORT  state=$STATE${NC}"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"

# ═══════════════════════════════════════════════════════════════
# 단계 1: daemon.conf admin_password=P1 로 부팅 → admin 시딩
# ═══════════════════════════════════════════════════════════════
write_conf "$P1"
if ! boot; then
    fail "A1: 격리 데몬이 REST /health 200 응답 실패"
    echo "---- daemon.log (마지막 30줄) ----"; tail -30 "$STATE/daemon.log" 2>/dev/null
    echo -e "\n${RED}데몬 기동 실패로 중단${NC}"; exit 1
fi
seed_ok=0
grep -q "Auto-created admin user 'admin'" "$STATE/daemon.log" && seed_ok=1
db_admin="$(sqlite3 "$RBAC_DB" "SELECT username FROM users WHERE username='admin';" 2>/dev/null || true)"
if [ "$seed_ok" = "1" ] && [ "$db_admin" = "admin" ]; then
    pass "A1: _ensure_admin_user 가 admin 시딩(로그 'Auto-created' + rbac.db users 행)"
else
    fail "A1: admin 시딩 확인 실패 (seed_log=$seed_ok db_row='$db_admin')"
fi

# ═══════════════════════════════════════════════════════════════
# 단계 2: 정상 로그인(P1) + 부트스트랩 admin change_password 특성
# ═══════════════════════════════════════════════════════════════
tok="$(login_token admin "$P1")"
if [ -n "$tok" ]; then
    pass "A2: login(admin, P1) → 200 + access_token"
else
    fail "A2: login(admin, P1) 토큰 발급 실패"
fi
# 부트스트랩 admin 은 REST change_password 로 self-rotate 불가(403)임을 문서화.
cp_code="$(curl -s -o /dev/null -w '%{http_code}' --max-time 6 -X POST "$BASE/auth/password" \
    -H "Authorization: Bearer $tok" -H 'Content-Type: application/json' \
    -d "{\"old_password\":\"$P1\",\"new_password\":\"RotatedNewPw2y8Z\"}" 2>/dev/null || echo 000)"
if [ "$cp_code" = "403" ]; then
    note "A2-finding: 부트스트랩 admin 은 REST /auth/password 로 회전 불가(403 FORBIDDEN). "\
"DB≠daemon.conf 발산은 설정 편집(부팅 시딩값 vs stale cfg_pass)으로 모델링한다."
else
    note "A2-finding: change_password(bootstrap admin) 응답=$cp_code (기대 403) — 별도 확인 필요"
fi

# ═══════════════════════════════════════════════════════════════
# 단계 3(핵심): cfg_pass 를 P0(stale)로 바꿔 재부팅 → 백도어 차단 실증
#   재부팅해도 admin 이 이미 존재하므로 _ensure 는 재시딩하지 않음
#   (create-only-if-absent) → DB=P1 유지, cfg_pass=P0.
# ═══════════════════════════════════════════════════════════════
stop
write_conf "$P0"
if ! boot; then
    fail "A3: P0 설정 재부팅 실패"; echo "---- daemon.log ----"; tail -20 "$STATE/daemon.log"
else
    c_p0="$(login_code admin "$P0")"   # cfg_pass 와 일치하지만 DB 부재 아님 → fallback 거부
    c_p1="$(login_code admin "$P1")"   # DB 비밀번호 → 정상 인증
    if [ "$c_p0" = "401" ] && [ "$c_p1" = "200" ]; then
        pass "A3(SEC-2 핵심): login(admin,P0=cfg_pass)→401 (fallback 거부), login(admin,P1=DB)→200"
    else
        fail "A3(SEC-2 핵심): P0=$c_p0 (기대 401), P1=$c_p1 (기대 200) — 백도어 미차단 가능"
    fi
fi

# ═══════════════════════════════════════════════════════════════
# 단계 4(관련버그): 재부팅 재시딩 여부 — create-only-if-absent 확인
#   회전(DB=P1) 상태에서 다시 재시작해도 P0(cfg_pass)가 통하면 안 됨.
# ═══════════════════════════════════════════════════════════════
stop
if ! boot; then
    fail "A5: 재시작 실패"; echo "---- daemon.log ----"; tail -20 "$STATE/daemon.log"
else
    c_p0b="$(login_code admin "$P0")"
    c_p1b="$(login_code admin "$P1")"
    if [ "$c_p0b" = "401" ] && [ "$c_p1b" = "200" ]; then
        pass "A5: 재부팅이 admin 을 P0 로 재시딩하지 않음 (P0→401, P1→200; create-only-if-absent)"
    elif [ "$c_p0b" = "200" ]; then
        fail "A5-SEPARATE-FINDING: 재부팅 후 P0(cfg_pass)→200 — _ensure 가 기존 admin 을 덮어씀(upsert). SEC-2와 별개 결함으로 기록"
    else
        fail "A5: 예상 밖 (P0=$c_p0b, P1=$c_p1b)"
    fi
fi

# ═══════════════════════════════════════════════════════════════
# 단계 5(첫설치 복구 보존): admin 부재 상태 재현 → fallback 200 + 감사
#   실행 중 데몬의 rbac.db 에서 admin 행 삭제(WAL 로 라이브 반영).
# ═══════════════════════════════════════════════════════════════
before="$(sqlite3 "$AUDIT_DB" "SELECT count(*) FROM audit_log WHERE method='auth.bootstrap.fallback';" 2>/dev/null || echo 0)"
sqlite3 "$RBAC_DB" "DELETE FROM users WHERE username='admin';" 2>/dev/null || true
gone="$(sqlite3 "$RBAC_DB" "SELECT count(*) FROM users WHERE username='admin';" 2>/dev/null || echo 1)"
sleep 0.5
c_fb="$(login_code admin "$P0")"   # 부재 + creds==cfg → fallback 발화
aud=0
for _ in $(seq 1 20); do
    n="$(sqlite3 "$AUDIT_DB" "SELECT count(*) FROM audit_log WHERE method='auth.bootstrap.fallback';" 2>/dev/null || echo 0)"
    [ "${n:-0}" -gt "${before:-0}" ] && { aud=1; break; }
    sleep 0.5
done
if [ "$gone" = "0" ] && [ "$c_fb" = "200" ] && [ "$aud" = "1" ]; then
    pass "A4: admin 부재 시 login(admin,P0)→200 via fallback + audit_log method='auth.bootstrap.fallback'"
    sqlite3 -header -column "$AUDIT_DB" \
        "SELECT username,method,target,result,src_ip FROM audit_log WHERE method='auth.bootstrap.fallback' ORDER BY ts DESC LIMIT 1;" 2>/dev/null || true
else
    fail "A4: fallback 복구 실증 실패 (admin_gone=$gone login=$c_fb audit_new=$aud)"
fi

stop

# ═══════════════════════════════════════════════════════════════
echo ""
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
printf "  결과: ${GREEN}PASS %d${NC} / ${RED}FAIL %d${NC}\n" "$PASS" "$FAIL"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
[ "$FAIL" -eq 0 ]
