#!/usr/bin/env bash

set -uo pipefail

GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS=0; FAIL=0
pass() { echo -e "${GREEN}[PASS]${NC} $*"; PASS=$((PASS+1)); }
fail() { echo -e "${RED}[FAIL]${NC} $*"; FAIL=$((FAIL+1)); }
info() { echo -e "${CYAN}[INFO]${NC} $*"; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
CAND_BIN="$REPO/bin/purecvisorsd"
DAEMON_BIN="${DAEMON_BIN:-$CAND_BIN}"

. "$SCRIPT_DIR/lib/pcv_isolated_daemon.sh"

skip() { echo -e "${YELLOW}[SKIP]${NC} 플러그인 API 내보내기 E2E: $*"; exit 0; }

command -v bwrap    >/dev/null 2>&1 || skip "bwrap(bubblewrap) 미설치"
command -v python3  >/dev/null 2>&1 || skip "python3 미설치"
command -v setsid   >/dev/null 2>&1 || skip "setsid 미설치"
command -v pkg-config >/dev/null 2>&1 || skip "pkg-config 미설치 — 시험 플러그인을 빌드할 수 없다"
CC="$(command -v gcc-14 || command -v cc || true)"
[ -n "$CC" ] || skip "C 컴파일러 미설치 — 시험 플러그인을 빌드할 수 없다"
command -v prlimit  >/dev/null 2>&1 || skip "prlimit 미설치 — 코어 덤프 억제 불가"
pcv_iso_capable || skip "$PCV_ISO_SKIP_REASON"

if [ ! -x "$DAEMON_BIN" ]; then
    info "데몬 바이너리 없음 — make daemon 시도"
    make -C "$REPO" daemon >/dev/null 2>&1 || true
fi
[ -x "$DAEMON_BIN" ] || skip "데몬 바이너리 빌드 실패 ($DAEMON_BIN)"
pcv_iso_check_bin "$DAEMON_BIN" || skip "$PCV_ISO_SKIP_REASON"
if [ "$DAEMON_BIN" -ef "$CAND_BIN" ]; then MODE=candidate; else MODE=counterfactual; fi

PAE_TMP="${TMPDIR:-/tmp}"
PAE_SOCK_SUFFIX="/pcv-pae.XXXXXX/var-lib/daemon.sock"
[ $(( ${#PAE_TMP} + ${#PAE_SOCK_SUFFIX} )) -le 100 ] || PAE_TMP=/tmp
STATE="$(mktemp -d "$PAE_TMP/pcv-pae.XXXXXX")" || skip "mktemp -d 실패 — 임시 상태 디렉터리를 만들 수 없다"
pcv_iso_check_state_fs "$STATE" || { rm -rf "$STATE"; exit 1; }
mkdir -p "$STATE/var-lib" "$STATE/etc"
SOCK="$STATE/var-lib/daemon.sock"
MARK="$STATE/var-lib/plugin-mark.txt"
LOG=""; BP=""; DPID=""; STOP_RC=""

cat > "$STATE/uds_call.py" <<'PYEOF'
import json, socket, sys
sock_path, method, params, tmo = sys.argv[1], sys.argv[2], sys.argv[3], float(sys.argv[4])
req = {"jsonrpc": "2.0", "method": method, "id": "pae"}
if params != "-":
    req["params"] = json.loads(params)
buf = b""
try:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(tmo)
    s.connect(sock_path)
    s.sendall(json.dumps(req).encode())
    while True:
        chunk = s.recv(65536)
        if not chunk:
            break
        buf += chunk
    s.close()
except Exception as e:
    print(f"ERR {e}")
    sys.exit(1)
sys.stdout.write(buf.decode(errors="replace"))
sys.exit(0 if buf.strip() else 1)
PYEOF

cat > "$STATE/check.py" <<'PYEOF'
import json, sys
lines = [l for l in open(sys.argv[1]).read().splitlines() if l.strip()]
try:
    objs = [json.loads(l) for l in lines]
except ValueError:
    print("not-json", lines); sys.exit(1)
n, r = len(objs), (objs[0] if objs else {})
ok = eval(sys.argv[2], {}, {"r": r, "n": n})
print(("OK " if ok else "MISMATCH ") + json.dumps(objs, ensure_ascii=False))
sys.exit(0 if ok else 1)
PYEOF

uds() { python3 -I "$STATE/uds_call.py" "$SOCK" "$1" "$2" 5; }
rpc_ok() { python3 -I "$STATE/uds_call.py" "$SOCK" daemon.version '{}' 3 >/dev/null 2>&1; }
alive() {
    local st
    [ -n "${1:-}" ] && [ -r "/proc/$1/stat" ] || return 1
    read -r _ _ st _ 2>/dev/null < "/proc/$1/stat" || return 1
    [ "$st" != "Z" ]
}
kill_daemon() {
    [ -n "${BP:-}" ] || return 0
    kill -9 -- "-$BP" 2>/dev/null || true
    kill -9 "$BP" 2>/dev/null || true
    for _ in $(seq 1 20); do alive "$BP" || break; sleep 0.25; done
    wait "$BP" 2>/dev/null || true
    BP=""; DPID=""
}

stop_daemon() {
    STOP_RC="timeout"
    [ -n "${BP:-}" ] || return 0
    if [ -n "${DPID:-}" ]; then
        kill -TERM "$DPID" 2>/dev/null || true
        for _ in $(seq 1 240); do alive "$DPID" || break; sleep 0.25; done
    fi
    if alive "${DPID:-}"; then kill_daemon; return 0; fi
    for _ in $(seq 1 40); do alive "$BP" || break; sleep 0.25; done
    if alive "$BP"; then kill_daemon; return 0; fi
    wait "$BP"; STOP_RC="$?"
    BP=""; DPID=""
}
cleanup() {
    kill_daemon
    if [ -n "${PCV_PAE_KEEP:-}" ]; then echo "임시 디렉터리 보존: $STATE"; else rm -rf "$STATE" 2>/dev/null || true; fi
}
trap cleanup EXIT
trap 'exit 130' INT; trap 'exit 143' TERM

mkdir -p "$STATE/etc/plugins.d"

if ! "$CC" -std=gnu23 -D_GNU_SOURCE -Wall -Wextra -Werror -shared -fPIC -I"$REPO/src" "$REPO/tests/fixtures/plugin_api_probe.c" \
        -o "$STATE/probe.so" $(pkg-config --cflags --libs glib-2.0 gio-2.0 json-glib-1.0) \
        > "$STATE/probe-build.log" 2>&1; then
    skip "시험 플러그인 빌드 실패: $(tail -2 "$STATE/probe-build.log" | tr '\n' ' ')"
fi
chmod 0644 "$STATE/probe.so"

chmod 0755 "$STATE/etc"
reset_plugins() {
    rm -rf "$STATE/etc/plugins.d"; mkdir -p "$STATE/etc/plugins.d"; chmod 0755 "$STATE/etc/plugins.d" "$STATE/etc"
    cp "$STATE/probe.so" "$STATE/etc/plugins.d/probe.so"; chmod 0644 "$STATE/etc/plugins.d/probe.so"
}
reset_plugins
pcv_iso_trusted_root_args
BW_ARGS=("${PCV_ISO_TRUSTED_ARGS[@]}")

cat > "$STATE/etc/daemon.conf" <<CONF
[daemon]
socket_path = /var/lib/purecvisor/daemon.sock
rest_port = 0
admin_user = admin
admin_password = PaeE2eFixedPw-not-for-prod
jwt_secret = pae-e2e-fixed-secret-not-for-prod-000000001
libvirt_uri = test:///default
log_level = info
drain_timeout = 5
CONF

boot() {
    local i
    LOG="$STATE/daemon.log"
    rm -f "$SOCK"
    setsid "$PCV_ISO_BWRAP" \
        "${BW_ARGS[@]}" \
        --bind "$STATE/var-lib" /var/lib/purecvisor \
        --bind "$STATE/etc" /etc/purecvisor \
        --setenv PATH /usr/sbin:/usr/bin:/sbin:/bin \
        --setenv PCV_TEST_PLUGIN_MARK /var/lib/purecvisor/plugin-mark.txt \
        --chdir / \
        prlimit --core=1:1 -- "$DAEMON_BIN" > "$LOG" 2>&1 < /dev/null &
    BP="$!"
    for ((i = 0; i < 120; i++)); do
        if [ -S "$SOCK" ] && rpc_ok; then
            DPID="$(pcv_iso_daemon_pid "$BP")"
            [ -n "$DPID" ] && return 0
        fi
        alive "$BP" || return 1
        sleep 0.5
    done
    return 1
}
count() { grep -cF -- "$1" "$LOG" 2>/dev/null || true; }

audit_line() { grep -F '"lvl":"AUDIT"' "$LOG" 2>/dev/null | grep -F "\"op\":\"$1\"" | grep -cF -- "$2" || true; }

expect() {
    local label="$1" out="$STATE/resp-$2.txt" res
    uds "$2" "$3" > "$out" 2>&1
    if res="$(python3 -I "$STATE/check.py" "$out" "$4" 2>&1)"; then pass "$label — $res"; else fail "$label — $res"; fi
}

echo "=== 플러그인 API 내보내기 E2E (MODE=$MODE) ==="
info "데몬 동적 정의 pcv_*: $(nm -D --defined-only "$DAEMON_BIN" 2>/dev/null | awk '{print $NF}' | grep '^pcv_' | tr '\n' ' ')"

reject_case() {
    local label="$1" code="$2" frag="$3" n
    if ! boot; then fail "$label: 격리 데몬 기동 실패 — $(tail -2 "$LOG" 2>/dev/null | tr '\n' ' ')"; kill_daemon; return; fi
    n="$(count 'Loaded plugin: pcv-test-api')"
    if [ "$n" -eq 0 ]; then pass "$label: 로딩 0"; else fail "$label: 로딩 ${n}줄(기대 0)"; fi
    n="$(grep -F '"lvl":"AUDIT"' "$LOG" | grep -F '"op":"plugin.load"' | grep -F "code=$code " | grep -cF -- "$frag" || true)"
    if [ "$n" -ge 1 ]; then pass "$label: 감사 plugin.load fail code $code ($frag)"
    else fail "$label: 감사 code $code·'$frag' 없음 — $(grep -F '"op":"plugin.load"' "$LOG" | tail -1 | cut -c1-260)"; fi
    expect "$label: 메서드 없음" pcvtestapi.echo '{}' 'n == 1 and r.get("error", {}).get("code") == -32601'
    stop_daemon
}
BW_ARGS=("${PCV_ISO_BWRAP_ARGS[@]}")
reject_case "① 기본 샌드박스(/etc 소유 65534)" 7 "/etc: owner uid 65534"
BW_ARGS=("${PCV_ISO_TRUSTED_ARGS[@]}")
chmod 0757 "$STATE/etc/plugins.d"
reject_case "② plugins.d o+w" 7 "plugins.d: group/other writable (mode 0757)"
reset_plugins; chmod 0757 "$STATE/etc"
reject_case "③ 상위 성분 /etc/purecvisor o+w" 7 "/etc/purecvisor: group/other writable (mode 0757)"
reset_plugins; chmod 0664 "$STATE/etc/plugins.d/probe.so"
reject_case "④ .so g+w" 8 "group/other writable (mode 0664)"
reset_plugins; mv "$STATE/etc/plugins.d/probe.so" "$STATE/etc/real-probe.so"; ln -s ../real-probe.so "$STATE/etc/plugins.d/probe.so"
reject_case "⑤ 심링크 .so" 6 "probe.so"
rm -f "$STATE/etc/real-probe.so"; reset_plugins

if boot; then
    n="$(count 'Loaded plugin: pcv-test-api')"
    if [ "$n" -eq 1 ]; then pass "시험 플러그인 로딩 1줄"; else fail "시험 플러그인 로딩 ${n}줄(기대 1): $(grep -F 'plugin' "$LOG" | tail -2 | tr '\n' ' ')"; fi
    n="$(audit_line plugin.load 'result=ok')"
    if [ "$n" -eq 1 ]; then pass "감사 plugin.load ok 1줄"; else fail "감사 plugin.load ok ${n}줄 — $(grep -F 'plugin.load' "$LOG" | tail -1 | cut -c1-240)"; fi
    n="$(count 'Plugin manager initialized (1 plugins, 6 methods)')"
    if [ "$n" -eq 1 ]; then pass "메서드 6개 등록"; else fail "Plugin manager initialized 줄: $(grep -o 'Plugin manager initialized ([^)]*)' "$LOG" | tail -1)"; fi

    expect "echo 성공 응답 한 줄" pcvtestapi.echo '{"msg":"hello"}' \
        'n == 1 and r.get("id") == "pae" and r.get("result") == {"plugin": "pcv-test-api", "echo": "hello"}'
    expect "fail 오류 응답 1001" pcvtestapi.fail '{}' \
        'n == 1 and r.get("error") == {"code": 1001, "message": "pcvtestapi failure requested"}'
    expect "twice 첫 응답만 한 줄" pcvtestapi.twice '{}' 'n == 1 and r.get("result") == "first"'
    expect "silent 데몬이 -32603 대신 응답" pcvtestapi.silent '{}' \
        'n == 1 and r.get("error") == {"code": -32603, "message": "plugin method returned without a response"}'
    expect "zero 코드 0 → -32603" pcvtestapi.zero '{}' 'n == 1 and r.get("error") == {"code": -32603, "message": "Unknown error"}'
    expect "info 성공 응답" pcvtestapi.info '{}' 'n == 1 and r.get("result") == {"plugin": "pcv-test-api", "info": True}'
    expect "params 없는 요청 → 처리기가 NULL 을 받아 echo null" pcvtestapi.echo - \
        'n == 1 and r.get("result") == {"plugin": "pcv-test-api", "echo": None}'
    expect "콜백 밖 등록 거절 → late 는 없는 메서드" pcvtestapi.late '{}' 'n == 1 and r.get("error", {}).get("code") == -32601'
    n="$(count "Plugin method 'pcvtestapi.late' rejected: pcv_plugin_registry_add called outside pcv_plugin_register")"

    if [ "$n" -eq 2 ]; then pass "콜백 밖 registry_add 거절 경고 2줄(echo 2회)"; else fail "콜백 밖 등록 거절 경고 ${n}줄(기대 2)"; fi
    n="$(count "Plugin method 'pcvtestapi.silent' (owner 'pcv-test-api') returned without a response")"
    if [ "$n" -eq 1 ]; then pass "응답 없는 처리기 경고 1줄"; else fail "응답 없는 처리기 경고 ${n}줄(기대 1)"; fi
    if grep -qx 'twice first=1 second=0 third=0' "$MARK" 2>/dev/null; then pass "두 번째·세 번째 응답 시도는 FALSE"; else fail "twice 표지: $(grep twice "$MARK" 2>/dev/null)"; fi

    for spec in "pcvtestapi.echo|result=ok code=0|2" "pcvtestapi.fail|result=fail code=1001|1" "pcvtestapi.twice|result=ok code=0|1" \
                "pcvtestapi.silent|result=fail code=-32603|1" "pcvtestapi.zero|result=fail code=-32603|1" "pcvtestapi.info|result=ok code=0|1"; do
        op="${spec%%|*}"; rest="${spec#*|}"; frag="${rest%|*}"; want="${rest##*|}"
        n="$(audit_line "$op" "$frag")"
        if [ "$n" -eq "$want" ]; then pass "감사 로그 $op $frag ${want}줄"; else fail "감사 로그 $op $frag ${n}줄(기대 $want) — $(grep -F "\"op\":\"$op\"" "$LOG" | tail -1 | cut -c1-240)"; fi
    done

    stop_daemon
    if [ "$STOP_RC" = 0 ]; then pass "SIGTERM 정상 종료 rc 0"; else fail "종료 rc $STOP_RC(기대 0)"; fi
    n="$(grep -c '^shutdown pid=' "$MARK" 2>/dev/null || true)"
    if [ "${n:-0}" -eq 1 ]; then pass "종료 콜백 표지 정확히 1줄"; else fail "종료 콜백 표지 ${n:-0}줄(기대 1)"; fi
    n="$(count 'Plugin manager shut down (1 plugins, 1 shutdown callbacks)')"
    if [ "$n" -eq 1 ]; then pass "Plugin manager shut down 1줄"; else fail "Plugin manager shut down ${n}줄(기대 1)"; fi

    DBQ="$(python3 -I - "$STATE/var-lib/pcv_audit.db" <<'PYEOF' 2>&1
import sqlite3, sys
con = sqlite3.connect(f"file:{sys.argv[1]}?mode=ro", uri=True)
rows = con.execute("SELECT method, username, result, error_code FROM audit_log "
                   "WHERE method LIKE 'pcvtestapi.%' OR method = 'plugin.load' ORDER BY id").fetchall()
for r in rows:
    print("|".join("" if v is None else str(v) for v in r))
PYEOF
)"
    echo "$DBQ" | sed 's/^/    db: /'
    for row in "plugin.load|system|ok|0" "pcvtestapi.fail|local-admin|fail|1001" \
               "pcvtestapi.twice|local-admin|ok|0" "pcvtestapi.silent|local-admin|fail|-32603" "pcvtestapi.zero|local-admin|fail|-32603" \
               "pcvtestapi.info|local-admin|ok|0" "pcvtestapi.late|local-admin|fail|-32601"; do
        n="$(printf '%s\n' "$DBQ" | grep -cxF -- "$row" || true)"
        if [ "$n" -eq 1 ]; then pass "감사 DB 행 $row"; else fail "감사 DB 행 $row ${n}개(기대 1)"; fi
    done

    for spec in "pcvtestapi.echo|local-admin|ok|0=2" "plugin.load|system|fail|7=3" "plugin.load|system|fail|8=1" "plugin.load|system|fail|6=1"; do
        row="${spec%=*}"; want="${spec##*=}"
        n="$(printf '%s\n' "$DBQ" | grep -cxF -- "$row" || true)"
        if [ "$n" -eq "$want" ]; then pass "감사 DB 행 $row ${want}개"; else fail "감사 DB 행 $row ${n}개(기대 $want)"; fi
    done
else
    fail "격리 데몬 기동 실패 — 로그: $(tail -3 "$LOG" 2>/dev/null | tr '\n' ' ')"; kill_daemon
fi
if [ "$MODE" = counterfactual ]; then
    info "반사실 로딩 줄: $(grep -F 'probe.so' "$LOG" 2>/dev/null | grep -o 'undefined symbol: [a-z_]*' | head -1)"
    info "반사실 감사: $(grep -F '"op":"plugin.load"' "$LOG" 2>/dev/null | grep -o 'result=[a-z]* code=[0-9-]*' | head -1)"
    info "MODE=counterfactual — 이 기능이 없는 바이너리면 로딩·호출 단언 FAIL 이 반사실 RED 다"
fi

echo
echo -e "PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ]
