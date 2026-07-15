#!/usr/bin/env bash
# tests/integration/test_session_revoke.sh
#
# SEC-1: 세션 revoke 강제 로그아웃 실동작 E2E (실/격리 데몬)
# ---------------------------------------------------------------------------
# 검증 대상: dispatcher.c 의 _handle_session_revoke(auth.session.revoke) 가
# 이제 토큰의 jti 를 pcv_jwt_verify 가 읽는 라이브 blacklist(g_jti_blacklist)에
# 등록한다(HEAD 82f9d4a, fix ab36168). 수정 전엔 죽은 rbac 세션 blacklist(호출처
# 0)에만 써서 revoke 된 토큰이 만료(900s)까지 계속 유효했다(SEC-1 무동작 결함).
#   - pcv_jwt_verify 는 blacklist 를 체크해 등록된 jti 를 거부한다(pcv_jwt.c:838).
#   - REST 는 모든 보호 라우트에서 _authenticate()→pcv_jwt_verify 를 태우므로,
#     revoke 후 같은 토큰의 인증 GET 은 401 을 받아야 한다.
#
# 증명 명제(핵심): 로그인→토큰 유효(200)→auth.session.revoke→같은 토큰 401.
#   수정 전엔 revoke 후에도 200(무동작). + audit_log.method='auth.session.revoke'.
#
# 경로 선택: SEC-1 은 "토큰 거부"가 증명 대상이므로 UDS 직결(admin, 토큰 불요)이
#   아니라 REST Bearer 토큰 경로로 검증한다(SEC-2 harness 관습 재사용).
#   - POST /api/v1/auth/token          → access_token(jti 포함)
#   - GET  /api/v1/auth/whoami         → 인증 GET (libvirt 무접촉, 순수 RBAC)
#   - POST /api/v1/auth/sessions/revoke {"jti":...} → auth.session.revoke RPC
#   - POST /api/v1/auth/logout         → 기존 logout 경로(무회귀)
#
# 격리 방식(SEC-2/CMP-1 harness 관습 재사용):
#   - bubblewrap(bwrap) 사용자 네임스페이스 uid-0 맵핑, /var/lib/purecvisor 와
#     /etc/purecvisor 를 mktemp 임시 디렉터리로 bind → 프로덕션 DB/소켓 완전 shadow.
#     rbac.db·pcv_audit.db 전부 임시 경로(하드코딩 경로는 bind 로만 격리 가능).
#   - admin 은 daemon.conf admin_password 로 시딩(_ensure_admin_user).
#   - libvirt 는 PCV_LIBVIRT_URI=test:///default (인메모리 mock, 호스트 무접촉).
#   - REST 는 비기본 고포트(127.0.0.1)에서 서빙, 짧은 테스트 창으로 최소화.
#
# 전제조건 부재 시(bwrap/sqlite3/python3/curl/setsid/userns 불가) 깨끗이 SKIP(exit 0).
#
# 실행: bash tests/integration/test_session_revoke.sh
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

skip() { echo -e "${YELLOW}[SKIP]${NC} SEC-1 E2E: $*"; exit 0; }

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
    for p in 28084 29084 31084 33084 37084 41084 43084; do
        if ! ss -ltn 2>/dev/null | grep -qE ":${p}([[:space:]]|$)"; then
            echo "$p"; return 0
        fi
    done
    return 1
}
PORT="$(pick_free_port)" || skip "빈 포트 확보 실패"
BASE="http://127.0.0.1:$PORT/api/v1"

# ── 자격증명 (테스트 전용, 프로덕션과 무관) ───────────────────────
ADMIN_PW='Sec1RevokeE2ePw-not-for-prod'

# ── 격리 상태 디렉터리 ────────────────────────────────────────────
STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-sec1.XXXXXX")"
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
jwt_secret = sec1-e2e-fixed-secret-not-for-prod-0001
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
login_token() { # $1=user $2=pass -> access_token or empty
    curl -s --max-time 6 -X POST "$BASE/auth/token" \
        -H 'Content-Type: application/json' \
        -d "{\"username\":\"$1\",\"password\":\"$2\"}" 2>/dev/null \
      | python3 -c "import sys,json;print(json.load(sys.stdin).get('access_token',''))" 2>/dev/null || true
}
auth_get_code() { # $1=token $2=path -> HTTP code (인증 GET)
    curl -s -o /dev/null -w '%{http_code}' --max-time 6 \
        -H "Authorization: Bearer $1" "$BASE/$2" 2>/dev/null || echo 000
}
extract_jti() { # $1=token -> jti (JWT payload.jti, base64url 디코딩)
    printf '%s' "$1" | python3 -c '
import sys, base64, json
tok = sys.stdin.read().strip()
try:
    payload = tok.split(".")[1]
    payload += "=" * ((4 - len(payload) % 4) % 4)   # base64url 패딩 보정
    obj = json.loads(base64.urlsafe_b64decode(payload))
    sys.stdout.write(obj.get("jti", ""))
except Exception:
    sys.stdout.write("")
' 2>/dev/null || true
}
revoke_call() { # $1=token $2=jti -> "HTTPCODE|body"
    curl -s -w '|%{http_code}' --max-time 6 -X POST "$BASE/auth/sessions/revoke" \
        -H "Authorization: Bearer $1" -H 'Content-Type: application/json' \
        -d "{\"jti\":\"$2\"}" 2>/dev/null || echo "|000"
}
logout_code() { # $1=token -> HTTP code
    curl -s -o /dev/null -w '%{http_code}' --max-time 6 -X POST "$BASE/auth/logout" \
        -H "Authorization: Bearer $1" -H 'Content-Type: application/json' \
        -d '{}' 2>/dev/null || echo 000
}

echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  SEC-1 세션 revoke 강제 로그아웃 실동작 E2E         ${NC}"
echo -e "${CYAN}  binary=$DAEMON_BIN${NC}"
echo -e "${CYAN}  port=$PORT  state=$STATE${NC}"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"

# ═══════════════════════════════════════════════════════════════
# 단계 1: 격리 데몬 기동 + 로그인 → 토큰 발급 + 초기 유효 확인
# ═══════════════════════════════════════════════════════════════
write_conf
if ! boot; then
    fail "R1: 격리 데몬이 REST /health 200 응답 실패"
    echo "---- daemon.log (마지막 30줄) ----"; tail -30 "$STATE/daemon.log" 2>/dev/null
    echo -e "\n${RED}데몬 기동 실패로 중단${NC}"; exit 1
fi
db_admin="$(sqlite3 "$RBAC_DB" "SELECT username FROM users WHERE username='admin';" 2>/dev/null || true)"
TOK="$(login_token admin "$ADMIN_PW")"
code_valid="$(auth_get_code "$TOK" "auth/whoami")"
if [ -n "$TOK" ] && [ "$code_valid" = "200" ] && [ "$db_admin" = "admin" ]; then
    pass "R1: 데몬 기동 + login(admin)→token + 초기 인증 GET /auth/whoami → $code_valid (유효)"
else
    fail "R1: token='${TOK:0:12}…' whoami=$code_valid admin_row='$db_admin' (기대 token+200+admin)"
    echo "---- daemon.log (마지막 20줄) ----"; tail -20 "$STATE/daemon.log" 2>/dev/null
    exit 1
fi

# ═══════════════════════════════════════════════════════════════
# 단계 2: jti 추출 → auth.session.revoke (핵심 배선)
# ═══════════════════════════════════════════════════════════════
JTI="$(extract_jti "$TOK")"
info "R2: 추출 jti=${JTI:-<empty>} (len=${#JTI})"
rv="$(revoke_call "$TOK" "$JTI")"
rv_body="${rv%|*}"; rv_code="${rv##*|}"
info "R2: revoke 응답 code=$rv_code body=${rv_body:-<empty>}"
if [ -n "$JTI" ] && [ "${#JTI}" = "32" ] && [ "$rv_code" = "200" ]; then
    pass "R2: jti(32 hex) 추출 + POST /auth/sessions/revoke → $rv_code ($rv_body)"
else
    fail "R2: jti='$JTI'(len=${#JTI}) revoke_code=$rv_code (기대 32-hex + 200)"
fi

# ═══════════════════════════════════════════════════════════════
# 단계 3(SEC-1 핵심): 취소된 토큰으로 인증 GET → 401 + 감사 기록
# ═══════════════════════════════════════════════════════════════
code_revoked="$(auth_get_code "$TOK" "auth/whoami")"
if [ "$code_revoked" = "401" ]; then
    pass "R3(SEC-1 핵심): revoke 후 같은 토큰 GET /auth/whoami → $code_revoked (거부)"
else
    fail "R3(SEC-1 핵심): revoke 후 GET /auth/whoami → $code_revoked (기대 401) — 수정 미적용/무동작"
fi
# 감사: auth.session.revoke 기록 확인 (라이브 WAL 반영 대기)
aud=0
for _ in $(seq 1 20); do
    n="$(sqlite3 "$AUDIT_DB" "SELECT count(*) FROM audit_log WHERE method='auth.session.revoke';" 2>/dev/null || echo 0)"
    [ "${n:-0}" -ge 1 ] && { aud=1; break; }
    sleep 0.3
done
if [ "$aud" = "1" ]; then
    pass "R3-audit: audit_log.method='auth.session.revoke' 기록 존재"
    # 참고: revoke 당 2행 기록 — (a) 핸들러 명시 감사(target=jti8, src_ip=local),
    # (b) dispatcher 의 제네릭 per-RPC 감사(NULL 필드). 아래는 (a) 결정적 표시.
    sqlite3 -header -column "$AUDIT_DB" \
        "SELECT username,method,target,result,src_ip FROM audit_log WHERE method='auth.session.revoke' AND target <> '' ORDER BY ts DESC LIMIT 1;" 2>/dev/null || true
else
    fail "R3-audit: audit_log 에 method='auth.session.revoke' 부재"
fi

# ═══════════════════════════════════════════════════════════════
# 단계 4(무회귀): 기존 logout 경로 — 새 토큰 → /auth/logout → 401
# ═══════════════════════════════════════════════════════════════
TOK2="$(login_token admin "$ADMIN_PW")"
code_v2="$(auth_get_code "$TOK2" "auth/whoami")"
lo_code="$(logout_code "$TOK2")"
code_after_logout="$(auth_get_code "$TOK2" "auth/whoami")"
info "R4: fresh_valid=$code_v2 logout=$lo_code after_logout=$code_after_logout"
if [ -n "$TOK2" ] && [ "$code_v2" = "200" ] && [ "$lo_code" = "200" ] && [ "$code_after_logout" = "401" ]; then
    pass "R4(무회귀): 새 토큰 유효(200) → POST /auth/logout(200) → 같은 토큰 401 (logout 경로 무회귀)"
else
    fail "R4(무회귀): fresh=$code_v2 logout=$lo_code after=$code_after_logout (기대 200/200/401)"
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
