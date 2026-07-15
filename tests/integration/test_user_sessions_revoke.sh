#!/usr/bin/env bash
# tests/integration/test_user_sessions_revoke.sh
#
# refresh-remint (SEC-1 후속): auth.user.sessions.revoke {username} 가 대상 사용자의
# refresh 세션 전부를 실제 취소해 access 토큰 재발급(re-mint)을 봉쇄하는지 E2E 검증.
# ---------------------------------------------------------------------------
# 검증 대상: dispatcher.c 의 _handle_user_sessions_revoke(auth.user.sessions.revoke)
# 가 pcv_rbac_revoke_session(username) 을 호출해 SQLite sessions.revoked=1 로 마킹하고,
# 그 결과 re-mint 경로(pcv_rbac_refresh_token, WHERE revoked=0 필터)가 실패한다.
#   - REST: POST /api/v1/auth/user-sessions/revoke  body {"username":"admin"}
#   - REST: POST /api/v1/auth/refresh               body {"refresh_token":...} → 401
#
# 증명 명제(핵심): 로그인(v2)→refresh 토큰 유효→user-sessions.revoke→같은
#   refresh 토큰으로 /auth/refresh 시 401(재발급 거부). 미배선/무호출이면 200(재발급).
#   + audit_log.method='auth.user.sessions.revoke' (핸들러 명시 감사; RBAC 레이어의
#     오명 'auth.session.revoke' 행과 별개 — 정명으로 단언).
#
# Deferred Minor(Task 4): admin-on-self만 커버하던 갭을 메우기 위해, admin이
#   non-admin(viewer) 사용자를 생성 → 그 사용자로 로그인 → 그 access 토큰으로
#   POST /auth/user-sessions/revoke {username:admin} 시도 → 403(FORBIDDEN) 단언을
#   추가한다. auth.user.sessions.revoke의 policy 최소 role은 ADMIN(dispatcher.c
#   g_method_policies)이므로 viewer 토큰은 거부되어야 한다.
#
# 주의: revoke 는 refresh 세션만 취소한다(access jti blacklist 아님). 그래서 revoke
#   호출에 쓴 access 토큰(access2)은 그대로 유효하고 revoke 자체는 200 이다.
#   또한 refresh 토큰 회전 때문에 revoke 전에 refresh2 를 소비하면 안 된다(캡처만).
#
# 격리 방식(SEC-1/CMP-1 harness 관습 재사용):
#   - bubblewrap(bwrap) 사용자 네임스페이스 uid-0 맵핑, /var/lib/purecvisor 와
#     /etc/purecvisor 를 mktemp 임시 디렉터리로 bind → 프로덕션 DB/소켓 완전 shadow.
#   - admin 은 daemon.conf admin_password 로 시딩(v2 authenticate → refresh 발급).
#   - libvirt 는 PCV_LIBVIRT_URI=test:///default (인메모리 mock, 호스트 무접촉).
#   - REST 는 비기본 고포트(127.0.0.1)에서 서빙.
#
# 전제조건 부재 시(bwrap/sqlite3/python3/curl/setsid/userns 불가) 깨끗이 SKIP(exit 0).
#
# 실행: bash tests/integration/test_user_sessions_revoke.sh
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

skip() { echo -e "${YELLOW}[SKIP]${NC} refresh-remint E2E: $*"; exit 0; }

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
    for p in 28086 29086 31086 33086 37086 41086 43086; do
        if ! ss -ltn 2>/dev/null | grep -qE ":${p}([[:space:]]|$)"; then
            echo "$p"; return 0
        fi
    done
    return 1
}
PORT="$(pick_free_port)" || skip "빈 포트 확보 실패"
BASE="http://127.0.0.1:$PORT/api/v1"

# ── 자격증명 (테스트 전용, 프로덕션과 무관) ───────────────────────
ADMIN_PW='RefreshRemintE2ePw-not-for-prod'

# ── 격리 상태 디렉터리 ────────────────────────────────────────────
STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-remint.XXXXXX")"
mkdir -p "$STATE/var-lib" "$STATE/etc"
RBAC_DB="$STATE/var-lib/rbac.db"
AUDIT_DB="$STATE/var-lib/pcv_audit.db"

kill_daemon() {  # graceful then SIGKILL fallback (세션 그룹 kill, pkill -f 미사용)
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
admin_password = $ADMIN_PW
jwt_secret = remint-e2e-fixed-secret-not-for-prod-0001
libvirt_uri = test:///default
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
        --setenv PCV_LIBVIRT_URI test:///default \
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

# ── REST 헬퍼 ─────────────────────────────────────────────────────
login_v2() { # $1=user $2=pass -> "ACCESS|REFRESH"
    curl -s --max-time 6 -X POST "$BASE/auth/token" \
        -H 'Content-Type: application/json' \
        -d "{\"username\":\"$1\",\"password\":\"$2\"}" 2>/dev/null \
      | python3 -c '
import sys, json
try:
    o = json.load(sys.stdin)
    sys.stdout.write(o.get("access_token", "") + "|" + o.get("refresh_token", ""))
except Exception:
    sys.stdout.write("|")
' 2>/dev/null || echo "|"
}
revoke_user_sessions() { # $1=access_token $2=target_username -> "HTTPCODE|body"
    curl -s -w '|%{http_code}' --max-time 6 -X POST "$BASE/auth/user-sessions/revoke" \
        -H "Authorization: Bearer $1" -H 'Content-Type: application/json' \
        -d "{\"username\":\"$2\"}" 2>/dev/null || echo "|000"
}
create_user() { # $1=admin_access_token $2=username $3=password $4=role -> "BODY|HTTPCODE"
    curl -s -w '|%{http_code}' --max-time 6 -X POST "$BASE/auth/users" \
        -H "Authorization: Bearer $1" -H 'Content-Type: application/json' \
        -d "{\"username\":\"$2\",\"password\":\"$3\",\"role\":\"$4\"}" 2>/dev/null || echo "|000"
}
refresh_code() { # $1=refresh_token -> HTTP code
    curl -s -o /dev/null -w '%{http_code}' --max-time 6 -X POST "$BASE/auth/refresh" \
        -H 'Content-Type: application/json' \
        -d "{\"refresh_token\":\"$1\"}" 2>/dev/null || echo 000
}

echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  refresh-remint 사용자 세션 취소 → re-mint 거부 E2E   ${NC}"
echo -e "${CYAN}  binary=$DAEMON_BIN${NC}"
echo -e "${CYAN}  port=$PORT  state=$STATE${NC}"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"

# ═══════════════════════════════════════════════════════════════
# 단계 1: 격리 데몬 기동 + admin 로그인(v2) → access2 + refresh2 캡처
#   (refresh2 는 revoke 전에 소비 금지 — 토큰 회전 방지. 캡처만.)
# ═══════════════════════════════════════════════════════════════
write_conf
if ! boot; then
    fail "R1: 격리 데몬이 REST /health 200 응답 실패"
    echo "---- daemon.log (마지막 30줄) ----"; tail -30 "$STATE/daemon.log" 2>/dev/null
    echo -e "\n${RED}데몬 기동 실패로 중단${NC}"; exit 1
fi
db_admin="$(sqlite3 "$RBAC_DB" "SELECT username FROM users WHERE username='admin';" 2>/dev/null || true)"
pair="$(login_v2 admin "$ADMIN_PW")"
ACCESS2="${pair%|*}"; REFRESH2="${pair##*|}"
info "R1: access2 len=${#ACCESS2}  refresh2 len=${#REFRESH2}  admin_row='$db_admin'"
if [ -n "$ACCESS2" ] && [ -n "$REFRESH2" ] && [ "$db_admin" = "admin" ]; then
    pass "R1: 데몬 기동 + login_v2(admin) → access2+refresh2 발급 + admin 시딩 확인"
else
    fail "R1: access2='${ACCESS2:0:12}…' refresh2='${REFRESH2:0:12}…' admin_row='$db_admin' (기대 둘다 발급+admin)"
    echo "---- daemon.log (마지막 20줄) ----"; tail -20 "$STATE/daemon.log" 2>/dev/null
    exit 1
fi

# ═══════════════════════════════════════════════════════════════
# 단계 1.5(RBAC, Deferred Minor): admin이 non-admin(viewer) 사용자 생성
#   → 로그인 → 그 토큰으로 user-sessions.revoke 시도 → 403 (ADMIN 정책 거부)
#   (admin-on-self만 커버하던 갭 — non-admin 거부를 실증)
# ═══════════════════════════════════════════════════════════════
NONADMIN_USER="remint_e2e_viewer"
NONADMIN_PW="NonAdminRemintE2ePw1"
cu="$(create_user "$ACCESS2" "$NONADMIN_USER" "$NONADMIN_PW" "viewer")"
cu_body="${cu%|*}"; cu_code="${cu##*|}"
info "R1b: auth.user.create(viewer, $NONADMIN_USER) 응답 code=$cu_code body=${cu_body:-<empty>}"
if [ "$cu_code" = "200" ] || [ "$cu_code" = "201" ]; then
    pass "R1b: admin이 non-admin(viewer) 사용자 '$NONADMIN_USER' 생성 성공"
else
    fail "R1b: 사용자 생성 실패 code=$cu_code (기대 200/201) — RBAC non-admin 시나리오 준비 불가"
fi

na_pair="$(login_v2 "$NONADMIN_USER" "$NONADMIN_PW")"
NA_ACCESS="${na_pair%|*}"
if [ -n "$NA_ACCESS" ]; then
    pass "R1c: non-admin(viewer) 로그인 성공 (access token 발급)"
else
    fail "R1c: non-admin(viewer) 로그인 실패 — access token 미발급"
fi

na_rv="$(revoke_user_sessions "$NA_ACCESS" "admin")"
na_rv_body="${na_rv%|*}"; na_rv_code="${na_rv##*|}"
info "R1d: non-admin(viewer) 토큰으로 revoke 시도 응답 code=$na_rv_code body=${na_rv_body:-<empty>}"
if [ "$na_rv_code" = "403" ]; then
    pass "R1d(핵심): non-admin(viewer) 토큰으로 user-sessions.revoke → 403 (RBAC ADMIN 정책 거부)"
else
    fail "R1d(핵심): non-admin(viewer) 토큰으로 user-sessions.revoke → $na_rv_code (기대 403) — RBAC 검사 미배선/우회"
fi

# 반사실 가드(Task 6): rest_server.c의 RBAC 사전검사 403 응답이 SOUP_MEMORY_STATIC +
# 하드코딩 길이(69, 실제 리터럴은 67바이트)라 인접 .rodata 2바이트를 over-read 해
# body 끝에 stray 바이트가 붙어 나갔었다. 위 na_rv_body 는 "$(...)" 캡처라 bash가
# NUL 바이트를 조용히 버리므로(실측: 이 over-read의 stray 바이트가 하필 0x00 0x00 인
# 빌드에서는 문자열 비교가 이를 통과시켜버림 — 반사실 검증 중 실측 확인) 바이트
# 단위 파일 비교로 재검증한다: 원본 body 를 디스크에 그대로 받아 기대 리터럴과
# cmp -s 로 정확 일치(길이+내용) 단언.
R1D_BODY_FILE="$STATE/r1d_body.bin"
R1D_EXPECT_FILE="$STATE/r1d_expected.bin"
curl -s -o "$R1D_BODY_FILE" --max-time 6 -X POST "$BASE/auth/user-sessions/revoke" \
    -H "Authorization: Bearer $NA_ACCESS" -H 'Content-Type: application/json' \
    -d '{"username":"admin"}' 2>/dev/null || true
printf '%s' '{"error":{"code":"FORBIDDEN","message":"Insufficient permissions"}}' > "$R1D_EXPECT_FILE"
if cmp -s "$R1D_BODY_FILE" "$R1D_EXPECT_FILE"; then
    pass "R1d-exact(반사실 가드): 403 body가 리터럴과 바이트 단위 정확 일치(over-read/절단 stray 바이트 없음)"
else
    got_len="$(wc -c < "$R1D_BODY_FILE" 2>/dev/null || echo '?')"
    want_len="$(wc -c < "$R1D_EXPECT_FILE" 2>/dev/null || echo '?')"
    got_hex="$(xxd -p "$R1D_BODY_FILE" 2>/dev/null | tr -d '\n')"
    fail "R1d-exact(반사실 가드): 403 body 불일치 — got_len=$got_len want_len=$want_len got_hex=$got_hex — SOUP_MEMORY_STATIC 길이 drift 재발 의심"
fi

# ═══════════════════════════════════════════════════════════════
# 단계 2: user-sessions.revoke {username:admin} (Bearer access2 — access 유효)
# ═══════════════════════════════════════════════════════════════
rv="$(revoke_user_sessions "$ACCESS2" "admin")"
rv_body="${rv%|*}"; rv_code="${rv##*|}"
info "R2: revoke 응답 code=$rv_code body=${rv_body:-<empty>}"
if [ "$rv_code" = "200" ]; then
    pass "R2: POST /auth/user-sessions/revoke {username:admin} → $rv_code (배선 확인)"
else
    fail "R2: revoke_code=$rv_code (기대 200) — 라우트/REST 미배선 또는 정책 거부"
fi

# ═══════════════════════════════════════════════════════════════
# 단계 3(핵심): 취소된 refresh 토큰으로 /auth/refresh → 401 (re-mint 거부)
# ═══════════════════════════════════════════════════════════════
ref_code="$(refresh_code "$REFRESH2")"
if [ "$ref_code" = "401" ]; then
    pass "R3(핵심): revoke 후 refresh2 로 POST /auth/refresh → $ref_code (재발급 거부)"
else
    fail "R3(핵심): revoke 후 POST /auth/refresh → $ref_code (기대 401) — re-mint 무동작/미배선"
fi

# ═══════════════════════════════════════════════════════════════
# 단계 4(감사): audit_log.method='auth.user.sessions.revoke' 기록 (라이브 WAL 대기)
# ═══════════════════════════════════════════════════════════════
aud=0
for _ in $(seq 1 20); do
    n="$(sqlite3 "$AUDIT_DB" "SELECT count(*) FROM audit_log WHERE method='auth.user.sessions.revoke';" 2>/dev/null || echo 0)"
    [ "${n:-0}" -ge 1 ] && { aud=1; break; }
    sleep 0.3
done
if [ "$aud" = "1" ]; then
    pass "R4-audit: audit_log.method='auth.user.sessions.revoke' 기록 존재(정명, RBAC 레이어 오명 auth.session.revoke 와 별개)"
    sqlite3 -header -column "$AUDIT_DB" \
        "SELECT username,method,target,result,src_ip FROM audit_log WHERE method='auth.user.sessions.revoke' ORDER BY ts DESC LIMIT 1;" 2>/dev/null || true
else
    fail "R4-audit: audit_log 에 method='auth.user.sessions.revoke' 부재"
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
