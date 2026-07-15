#!/usr/bin/env bash
# tests/integration/test_json_ingress_disp1.sh
#
# 게이트 #2 (JSON ingress / DISP-1): 외부 JSON 파싱 경계 크래시0 + 경계 거부 E2E
# ---------------------------------------------------------------------------
# 검증 대상: 모든 외부 JSON 파싱 경계가 pcv_rpc_parse_guarded(깊이≤128, 크기≤1MB를
# 파싱 *전*에 선검사)를 경유한다(HEAD 015ae80, 브랜치 audit-remediation-gates).
#   - src/api/ws_server.c  _on_ws_message: WS 사전인증 텍스트 프레임 (=DISP-1).
#       수정 전엔 깊게 중첩된 미인증 프레임이 json-glib 재귀 파서 스택오버플로우로
#       데몬을 크래시시켰다. 이제 깊이 가드가 파싱 전에 거부 → 크래시 불가.
#   - src/api/rest_server.c _parse_body: HTTP body (Task 4 외부 마이그레이션).
#   - src/api/dispatcher.c  request entry.
#
# 증명 명제(핵심, DISP-1): 신선한 사전인증 WS 연결에 깊은 중첩 텍스트 프레임을
# 보내도 데몬이 크래시하지 않는다(pid 생존 + 이후 REST 계속 서빙). 정상 인증
# 프레임({"type":"auth","token":JWT})은 여전히 accept 된다(래퍼가 정상 경로 무회귀).
# 추가: 과대(>1MB) 프레임 사전인증 → 생존. REST 정상 로그인 무회귀 + 적대적 body
# (>128 깊이, >1MB) → 데몬 거부(에러 상태) + 생존.
#
# WS 클라이언트: python3 `websockets`(sync) 모듈로 RFC6455 핸드셰이크 + 마스킹
# 텍스트 프레임 전송. 모듈 부재 시 깨끗이 SKIP.
#
# 격리 방식(SEC-2/CMP-1/SEC-1 harness 관습 재사용):
#   - bubblewrap(bwrap) 사용자 네임스페이스 uid-0 맵핑, /var/lib/purecvisor 와
#     /etc/purecvisor 를 mktemp 임시 디렉터리로 bind → 프로덕션 DB/소켓 완전 shadow.
#   - libvirt 는 PCV_LIBVIRT_URI=test:///default (인메모리 mock, 호스트 무접촉).
#   - REST/WS 는 비기본 고포트(127.0.0.1)에서 서빙, 짧은 테스트 창으로 최소화.
#
# 전제조건 부재 시(bwrap/python3+websockets/curl/setsid/userns 불가) 깨끗이 SKIP(exit 0).
#
# 실행: bash tests/integration/test_json_ingress_disp1.sh
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

skip() { echo -e "${YELLOW}[SKIP]${NC} 게이트#2/DISP-1 E2E: $*"; exit 0; }

# ── 전제조건 검사 ────────────────────────────────────────────────
command -v bwrap    >/dev/null 2>&1 || skip "bwrap(bubblewrap) 미설치"
command -v curl     >/dev/null 2>&1 || skip "curl 미설치"
command -v python3  >/dev/null 2>&1 || skip "python3 미설치"
command -v setsid   >/dev/null 2>&1 || skip "setsid 미설치"
python3 -c 'import websockets' >/dev/null 2>&1 || skip "python3 websockets 모듈 미설치 (WS 클라이언트 불가)"

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
WS_URL="ws://127.0.0.1:$PORT/api/v1/ws/events"

# ── 자격증명 (테스트 전용, 프로덕션과 무관) ───────────────────────
ADMIN_PW='Disp1JsonE2ePw-not-for-prod'

# 깊이 / 크기 (가드 상수: 깊이 128, 크기 1MB)
DEEP_N=100000            # >128, 단일 WS 텍스트 프레임(≈97.6KiB, libsoup 128KiB 프레임 상한 내)
OVERSIZE=1258291         # >1MB (1.2MiB) — 과대 입력

# ── 격리 상태 디렉터리 ────────────────────────────────────────────
STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-disp1.XXXXXX")"
mkdir -p "$STATE/var-lib" "$STATE/etc"

# ── WS 클라이언트 헬퍼 (python3 websockets sync) ──────────────────
cat > "$STATE/ws_client.py" <<'PYEOF'
import sys, json, time
from websockets.sync.client import connect

mode = sys.argv[1]
url  = sys.argv[2]

def open_conn():
    # max_size=None: 클라이언트측 수신 상한 해제(전송에는 상한 없음)
    return connect(url, open_timeout=8, close_timeout=3, max_size=None)

try:
    if mode == "auth":
        token = sys.argv[3]
        with open_conn() as ws:
            ws.send(json.dumps({"type": "auth", "token": token}))
            try:
                resp = ws.recv(timeout=5)
            except Exception as e:
                print("NORESP:%s" % e); sys.exit(3)
            print("RECV:%s" % resp)
            sys.exit(0 if '"auth_ok"' in str(resp) else 4)

    elif mode == "deep":
        depth = int(sys.argv[3])
        frame = "[" * depth
        with open_conn() as ws:
            ws.send(frame)
            time.sleep(0.5)   # 서버가 프레임을 처리(가드 거부)할 시간
            print("SENT_DEEP:%d bytes" % len(frame))
            sys.exit(0)

    elif mode == "big":
        nbytes = int(sys.argv[3])
        frame = '["' + ("A" * (nbytes - 4)) + '"]'
        with open_conn() as ws:
            ws.send(frame)
            time.sleep(0.3)
            print("SENT_BIG:%d bytes" % len(frame))
            sys.exit(0)
    else:
        print("BADMODE"); sys.exit(9)
except SystemExit:
    raise
except Exception as e:
    # 서버가 프레임 거부 후 연결을 닫으면 send/close 에서 예외가 날 수 있다.
    # 이는 정상(사전인증 거부). 데몬 생존은 스크립트가 별도로 REST 로 확인한다.
    print("WSERR:%s" % e)
    # deep/big 은 거부가 기대되므로 0, auth 는 실패로 취급
    sys.exit(0 if mode in ("deep", "big") else 2)
PYEOF

ws_client() { python3 "$STATE/ws_client.py" "$@" 2>&1; }

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
jwt_secret = disp1-e2e-fixed-secret-not-for-prod-0001
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

daemon_pid() { cat "$STATE/bwrap.pid" 2>/dev/null || echo ""; }
daemon_alive() { local bp; bp="$(daemon_pid)"; [ -n "$bp" ] && kill -0 "$bp" 2>/dev/null; }
health_code() { curl -s -o /dev/null -w '%{http_code}' --max-time 4 "$BASE/health" 2>/dev/null || echo 000; }

login_token() { # $1=user $2=pass -> access_token or empty
    curl -s --max-time 6 -X POST "$BASE/auth/token" \
        -H 'Content-Type: application/json' \
        -d "{\"username\":\"$1\",\"password\":\"$2\"}" 2>/dev/null \
      | python3 -c "import sys,json;print(json.load(sys.stdin).get('access_token',''))" 2>/dev/null || true
}
login_code() { # $1=user $2=pass -> HTTP code
    curl -s -o /dev/null -w '%{http_code}' --max-time 6 -X POST "$BASE/auth/token" \
        -H 'Content-Type: application/json' \
        -d "{\"username\":\"$1\",\"password\":\"$2\"}" 2>/dev/null || echo 000
}
auth_get_code() { # $1=token $2=path -> HTTP code
    curl -s -o /dev/null -w '%{http_code}' --max-time 6 \
        -H "Authorization: Bearer $1" "$BASE/$2" 2>/dev/null || echo 000
}
# POST raw body from file to /auth/token, return HTTP code
post_body_file_code() { # $1=path-suffix $2=file
    curl -s -o /dev/null -w '%{http_code}' --max-time 8 -X POST "$BASE/$1" \
        -H 'Content-Type: application/json' --data-binary "@$2" 2>/dev/null || echo 000
}

echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  게이트#2 JSON ingress / DISP-1 크래시0 E2E         ${NC}"
echo -e "${CYAN}  binary=$DAEMON_BIN${NC}"
echo -e "${CYAN}  port=$PORT  ws=$WS_URL${NC}"
echo -e "${CYAN}  state=$STATE${NC}"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"

# ═══════════════════════════════════════════════════════════════
# 단계 1: 격리 데몬 기동 + 서빙 확인 + 로그인 토큰
# ═══════════════════════════════════════════════════════════════
write_conf
if ! boot; then
    fail "A1: 격리 데몬이 REST /health 200 응답 실패"
    echo "---- daemon.log (마지막 30줄) ----"; tail -30 "$STATE/daemon.log" 2>/dev/null
    echo -e "\n${RED}데몬 기동 실패로 중단${NC}"; exit 1
fi
TOK="$(login_token admin "$ADMIN_PW")"
if [ -n "$TOK" ] && daemon_alive; then
    pass "A1: 격리 데몬 기동 + REST /health 200 + login(admin)→token (서빙 확인)"
else
    fail "A1: token='${TOK:0:12}…' alive=$(daemon_alive && echo yes || echo no)"
    echo "---- daemon.log (마지막 20줄) ----"; tail -20 "$STATE/daemon.log" 2>/dev/null
    exit 1
fi

# ═══════════════════════════════════════════════════════════════
# 단계 2: 정상 auth 프레임 → auth_ok (래퍼가 정상 경로 무회귀)
# ═══════════════════════════════════════════════════════════════
auth_out="$(ws_client auth "$WS_URL" "$TOK")"; auth_rc=$?
info "A2: WS auth 응답 = ${auth_out}"
if [ "$auth_rc" = "0" ]; then
    pass "A2: WS 사전인증 연결에 {\"type\":\"auth\",token} 전송 → auth_ok (정상 인증 무회귀)"
else
    fail "A2: WS auth 프레임이 auth_ok 를 받지 못함 (rc=$auth_rc out='$auth_out')"
fi

# ═══════════════════════════════════════════════════════════════
# 단계 3 (DISP-1 핵심): 깊은 중첩 프레임 → 데몬 크래시 없음
# ═══════════════════════════════════════════════════════════════
deep_out="$(ws_client deep "$WS_URL" "$DEEP_N")"; deep_rc=$?
info "A3: WS deep($DEEP_N) 전송 결과 = ${deep_out} (rc=$deep_rc)"
sleep 0.5
alive_after_deep=no; hc_after_deep=000
daemon_alive && alive_after_deep=yes
hc_after_deep="$(health_code)"
if [ "$alive_after_deep" = "yes" ] && [ "$hc_after_deep" = "200" ]; then
    pass "A3(DISP-1 핵심): 깊은 중첩($DEEP_N) 사전인증 WS 프레임 후 데몬 생존(pid alive) + 계속 서빙(health=$hc_after_deep)"
else
    fail "A3(DISP-1 핵심): deep 프레임 후 데몬 crash 의심 (alive=$alive_after_deep health=$hc_after_deep) — 가드 미적용/스택오버플로우"
    echo "---- daemon.log (마지막 20줄) ----"; tail -20 "$STATE/daemon.log" 2>/dev/null
fi

# ═══════════════════════════════════════════════════════════════
# 단계 4: 과대(>1MB) 프레임 사전인증 → 데몬 생존
# ═══════════════════════════════════════════════════════════════
big_out="$(ws_client big "$WS_URL" "$OVERSIZE")"; big_rc=$?
info "A4: WS big($OVERSIZE) 전송 결과 = ${big_out} (rc=$big_rc)"
sleep 0.5
alive_after_big=no; hc_after_big=000
daemon_alive && alive_after_big=yes
hc_after_big="$(health_code)"
if [ "$alive_after_big" = "yes" ] && [ "$hc_after_big" = "200" ]; then
    pass "A4: 과대(>1MB) 사전인증 WS 프레임 후 데몬 생존 + 계속 서빙(health=$hc_after_big)"
else
    fail "A4: big 프레임 후 데몬 crash 의심 (alive=$alive_after_big health=$hc_after_big)"
    echo "---- daemon.log (마지막 20줄) ----"; tail -20 "$STATE/daemon.log" 2>/dev/null
fi

# ═══════════════════════════════════════════════════════════════
# 단계 5 (REST 무회귀): 정상 로그인 + 인증 GET
# ═══════════════════════════════════════════════════════════════
lg_code="$(login_code admin "$ADMIN_PW")"
who_code="$(auth_get_code "$TOK" "auth/whoami")"
if [ "$lg_code" = "200" ] && [ "$who_code" = "200" ]; then
    pass "A5(REST 무회귀): /auth/token→$lg_code + 인증 GET /auth/whoami→$who_code (정상 파싱 무회귀)"
else
    fail "A5(REST 무회귀): login=$lg_code whoami=$who_code (기대 200/200)"
fi

# ═══════════════════════════════════════════════════════════════
# 단계 6 (REST 적대적 body): 깊은 중첩 + 과대 body → 거부 + 생존
# ═══════════════════════════════════════════════════════════════
# 6a: 깊은 중첩 body (>128) → guard 거부 → _parse_body NULL → 에러 상태
python3 -c "import sys; sys.stdout.write('['*$DEEP_N)" > "$STATE/deep_body.json"
rest_deep_code="$(post_body_file_code "auth/token" "$STATE/deep_body.json")"
# 6b: 유효 자격증명을 깊은 중첩으로 감싼 body → guard 거부 → 200 아님(판별력)
python3 -c "
import sys
pw='$ADMIN_PW'
sys.stdout.write('{\"username\":\"admin\",\"password\":\"'+pw+'\",\"pad\":'+'['*200+']'*200+'}')
" > "$STATE/deep_valid_body.json"
rest_deepvalid_code="$(post_body_file_code "auth/token" "$STATE/deep_valid_body.json")"
# 6c: 과대 body (>1MB) → REST_MAX_BODY/guard 거부 → 에러 상태
python3 -c "import sys; sys.stdout.write('[\"'+'A'*($OVERSIZE-4)+'\"]')" > "$STATE/big_body.json"
rest_big_code="$(post_body_file_code "auth/token" "$STATE/big_body.json")"
info "A6: rest_deep=$rest_deep_code rest_deep+validcreds=$rest_deepvalid_code rest_big=$rest_big_code"

alive_after_rest=no; hc_after_rest=000
daemon_alive && alive_after_rest=yes
hc_after_rest="$(health_code)"
# 정상 로그인 재확인(적대적 body 이후에도 무회귀)
lg_after="$(login_code admin "$ADMIN_PW")"

# 판정: (a) 세 적대적 body 모두 2xx 아님, (b) 특히 유효자격+깊은중첩이 200 아님(가드 실동작),
#       (c) 데몬 생존 + health 200, (d) 이후 정상 로그인 200
rest_deep_nz=0;   case "$rest_deep_code"      in 2*) rest_deep_nz=0 ;; *) rest_deep_nz=1 ;; esac
rest_dv_nz=0;     case "$rest_deepvalid_code" in 2*) rest_dv_nz=0 ;;   *) rest_dv_nz=1 ;; esac
rest_big_nz=0;    case "$rest_big_code"       in 2*) rest_big_nz=0 ;;  *) rest_big_nz=1 ;; esac
if [ "$rest_deep_nz" = "1" ] && [ "$rest_dv_nz" = "1" ] && [ "$rest_big_nz" = "1" ] \
   && [ "$alive_after_rest" = "yes" ] && [ "$hc_after_rest" = "200" ] && [ "$lg_after" = "200" ]; then
    pass "A6(REST 적대적): 깊은($rest_deep_code)/유효자격+깊은($rest_deepvalid_code)/과대($rest_big_code) body 모두 거부(비2xx) + 데몬 생존(health=$hc_after_rest) + 이후 정상 로그인 $lg_after"
else
    fail "A6(REST 적대적): deep=$rest_deep_code deepvalid=$rest_deepvalid_code big=$rest_big_code alive=$alive_after_rest health=$hc_after_rest login_after=$lg_after"
fi

# ═══════════════════════════════════════════════════════════════
# 단계 7 (생존): 모든 적대적 입력 후 데몬이 정상 로그인 서빙
# ═══════════════════════════════════════════════════════════════
final_login="$(login_code admin "$ADMIN_PW")"
final_alive=no; daemon_alive && final_alive=yes
if [ "$final_alive" = "yes" ] && [ "$final_login" = "200" ]; then
    pass "A7(생존): 깊은/과대 WS + 깊은/과대 REST 적대적 입력 전량 이후 데몬 생존 + 정상 로그인 $final_login (크래시0)"
else
    fail "A7(생존): 최종 alive=$final_alive login=$final_login (기대 yes/200)"
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
