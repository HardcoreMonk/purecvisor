#!/usr/bin/env bash

set -uo pipefail

GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS=0; FAIL=0
pass() { echo -e "${GREEN}[PASS]${NC} $*"; PASS=$((PASS+1)); }
fail() { echo -e "${RED}[FAIL]${NC} $*"; FAIL=$((FAIL+1)); }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
DAEMON_BIN="${PCV_DAEMON_BIN:-$REPO/bin/purecvisorsd}"
PCVCTL_BIN="$REPO/bin/pcvctl"

. "$SCRIPT_DIR/lib/pcv_isolated_daemon.sh"

skip() { echo -e "${YELLOW}[SKIP]${NC} contract drift CLI 격리 E2E: $*"; exit 0; }

for tool in bwrap python3 jq setsid; do
    command -v "$tool" >/dev/null 2>&1 || skip "$tool 미설치"
done
pcv_iso_capable || skip "$PCV_ISO_SKIP_REASON"
if [ ! -x "$DAEMON_BIN" ] || [ ! -x "$PCVCTL_BIN" ]; then
    make -C "$REPO" daemon cli >/dev/null 2>&1 || true
fi
[ -x "$DAEMON_BIN" ] || skip "데몬 바이너리 빌드 실패 ($DAEMON_BIN)"
[ -x "$PCVCTL_BIN" ] || skip "pcvctl 바이너리 빌드 실패 ($PCVCTL_BIN)"
pcv_iso_check_bin "$DAEMON_BIN" || skip "$PCV_ISO_SKIP_REASON"

KEY='fixture-e2e-key-not-a-real-secret-42'
STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-driftcli.XXXXXX")"
pcv_iso_check_state_fs "$STATE" || { rm -rf "$STATE"; exit 1; }
mkdir -p "$STATE/var-lib" "$STATE/etc"
SOCK="$STATE/var-lib/daemon.sock"
DLOG="$STATE/daemon.log"
cat > "$STATE/etc/daemon.conf" <<CONF
[daemon]
socket_path = /var/lib/purecvisor/daemon.sock
rest_port = 28331
admin_user = admin
admin_password = DriftCliE2eAdminPw-not-for-prod
jwt_secret = drift-cli-e2e-fixed-secret-not-for-prod-01
libvirt_uri = test:///default
log_level = info
drain_timeout = 1
CONF

kill_daemon() {
    local f="$STATE/bwrap.pid" bp
    [ -f "$f" ] || return 0
    bp="$(cat "$f" 2>/dev/null || true)"
    [ -n "${bp:-}" ] || return 0
    kill -- "-$bp" 2>/dev/null || true
    kill "$bp" 2>/dev/null || true
    for _ in $(seq 1 12); do
        kill -0 "$bp" 2>/dev/null || break
        sleep 0.25
    done
    kill -9 -- "-$bp" 2>/dev/null || true
    kill -9 "$bp" 2>/dev/null || true
    rm -f "$f"
}
cleanup() { kill_daemon; rm -rf "$STATE" 2>/dev/null || true; }
trap cleanup EXIT INT TERM

pcvctl_iso() {
    [ -S "$SOCK" ] || return 125
    "$PCVCTL_BIN" --socket="$SOCK" --no-color "$@"
}
uds() {
    printf '%s' "{\"jsonrpc\":\"2.0\",\"method\":\"$1\",\"params\":$2,\"id\":\"drift\"}" \
        | pcv_iso_uds_nc "$SOCK" 15 2>/dev/null || true
}
ecode() { printf '%s' "$1" | jq -r '.error.code // empty' 2>/dev/null; }

prov() { uds agent.config.get '{}' | jq -r --arg n "$1" ".result.providers[] | select(.name==\$n) | $2" 2>/dev/null; }

setsid "$PCV_ISO_BWRAP" \
    "${PCV_ISO_BWRAP_ARGS[@]}" \
    --bind "$STATE/var-lib" /var/lib/purecvisor \
    --bind "$STATE/etc" /etc/purecvisor \
    --chdir / \
    "$DAEMON_BIN" > "$DLOG" 2>&1 < /dev/null &
echo "$!" > "$STATE/bwrap.pid"
ready=0
for _ in $(seq 1 60); do
    if [ -S "$SOCK" ] && [ -n "$(prov Claude .name)" ]; then ready=1; break; fi
    sleep 0.5
done
if [ "$ready" != 1 ]; then
    fail "S1: 격리 데몬 기동 실패"
    tail -30 "$DLOG" 2>/dev/null
    exit 1
fi
pass "S1: 격리 데몬 기동 + agent.config.get 응답"

out="$(pcvctl_iso agent set --provider claude --api_key "$KEY" --model e2e-model \
       --endpoint https://example.invalid/v1 2>&1)"; rc=$?
if [ "$rc" = 0 ] && [ "$(prov Claude .enabled)" = true ] && [ "$(prov Claude .model)" = e2e-model ] \
   && [ "$(prov Claude .endpoint)" = https://example.invalid/v1 ]; then
    pass "A1: agent set → rc 0, enabled·model·endpoint 반영"
else
    fail "A1: agent set rc=$rc enabled=$(prov Claude .enabled) model=$(prov Claude .model) endpoint=$(prov Claude .endpoint)"
fi
cfg="$(pcvctl_iso --format=json agent config 2>&1)"
case "$out$cfg" in
    *"$KEY"*) fail "A1b: pcvctl 출력에 키 원문" ;;
    *) pass "A1b: pcvctl 출력에 키 원문 없음(응답은 가려진 값)" ;;
esac

pcvctl_iso agent set --provider claude --api_key "" >/dev/null 2>&1; rc=$?
if [ "$rc" = 0 ] && [ "$(prov Claude .enabled)" = false ]; then pass "A2: --api_key \"\" → enabled false"
else fail "A2: rc=$rc enabled=$(prov Claude .enabled)"; fi

r="$(uds agent.config.set "{\"providers\":[{\"name\":\"OpenAI\",\"model\":\"ui-model\",\"api_key\":\"$KEY\",\"endpoint\":\"https://example.invalid/o\",\"enabled\":true}]}")"
if [ -z "$(ecode "$r")" ] && [ "$(prov OpenAI .enabled)" = true ] && [ "$(prov OpenAI .model)" = ui-model ]; then
    pass "A3: 웹 UI 배열 → 성공, 설정 반영"
else
    fail "A3: 코드=$(ecode "$r") enabled=$(prov OpenAI .enabled)"
fi

before="$(uds agent.config.get '{}' | jq -cS '.result.providers' 2>/dev/null)"
for bad in '{"providers":{"Gemini":{"api_key":"x"}}}' '{"providers":"x"}' '{"providers":null}' \
           '{"providers":[1]}' '{"providers":[{"name":"Gemini","api_key":"x"},"y"]}' '{"rate_limit_sec":5}'; do
    r="$(uds agent.config.set "$bad")"
    if [ "$(ecode "$r")" = "-32602" ]; then pass "A4: $bad → -32602"
    else fail "A4: $bad → 코드 '$(ecode "$r")' (기대 -32602)"; fi
done
if [ "$(uds agent.config.get '{}' | jq -cS '.result.providers' 2>/dev/null)" = "$before" ]; then
    pass "A4b: 거절된 요청이 설정을 바꾸지 않음"
else
    fail "A4b: 거절된 요청 뒤 설정이 달라짐"
fi

for odd in '{"providers":[{"name":1,"api_key":"x"}]}' '{"providers":[{"name":"Gemini","api_key":[],"model":{}}]}'; do
    r="$(uds agent.config.set "$odd")"
    if [ -z "$(ecode "$r")" ] && [ "$(uds agent.config.get '{}' | jq -cS '.result.providers' 2>/dev/null)" = "$before" ]; then
        pass "A4c: $odd → 변경 없음"
    else
        fail "A4c: $odd → 코드 '$(ecode "$r")', 설정이 달라졌거나 거절됨"
    fi
done

for name in --name -x -; do
    r="$(uds container.create "{\"name\":\"$name\"}")"
    if [ "$(ecode "$r")" = "-32602" ]; then pass "C1: container.create name=$name → -32602"
    else fail "C1: container.create name=$name → 코드 '$(ecode "$r")' (기대 -32602)"; fi
done
r="$(uds container.clone '{"source":"origin-ctr","dest":"-x"}')"
if [ "$(ecode "$r")" = "-32602" ]; then pass "C2: container.clone dest=-x → -32602"
else fail "C2: container.clone dest=-x → 코드 '$(ecode "$r")' (기대 -32602)"; fi

r="$(uds container.metrics '{"name":"--name"}')"
if [ "$(ecode "$r")" != "-32602" ]; then pass "C3: container.metrics name=--name → 이름 판정 통과(코드 '$(ecode "$r")')"
else fail "C3: container.metrics name=--name → -32602"; fi

pcvctl_iso --format=json storage pool list >/dev/null 2>&1; rc=$?
if [ "$rc" != 2 ]; then pass "D1a: storage pool list → 사용법 오류 아님(rc $rc)"
else fail "D1a: storage pool list → rc 2"; fi
out="$(pcvctl_iso --format=json device disk attach no-such-vm --source /dev/null --target vdb 2>&1)"; rc=$?
if [ "$rc" = 1 ] && [ "$(ecode "$out")" != "-32602" ]; then pass "D1b: device disk attach → 핸들러 판정(코드 '$(ecode "$out")')"
else fail "D1b: device disk attach → rc $rc 코드 '$(ecode "$out")'"; fi
out="$(pcvctl_iso --format=json device disk detach no-such-vm --target vdb 2>&1)"; rc=$?
if [ "$rc" = 1 ] && [ "$(ecode "$out")" != "-32602" ]; then pass "D1c: device disk detach → 핸들러 판정(코드 '$(ecode "$out")')"
else fail "D1c: device disk detach → rc $rc 코드 '$(ecode "$out")'"; fi

for fmt in table plain; do
    out="$(pcvctl_iso --format=$fmt snapshot list test 2>&1)"; rc=$?
    case "$out" in
        *"(null)"*) fail "D1d: snapshot list/$fmt → (null) 출력" ;;
        *) if [ "$rc" = 0 ]; then pass "D1d: snapshot list/$fmt → rc 0, (null) 없음"
           else fail "D1d: snapshot list/$fmt → rc $rc"; fi ;;
    esac
done

kill_daemon

n="$(grep -c '"dom":"Json"' "$DLOG" 2>/dev/null || true)"
if [ "${n:-0}" = 0 ]; then pass "A5: 데몬 로그 json-glib critical 0줄"
else fail "A5: 데몬 로그 json-glib critical ${n}줄"; fi
if grep -qrF "$KEY" "$STATE" 2>/dev/null; then fail "A5b: 데몬 로그·상태 파일에 키 원문"
else pass "A5b: 데몬 로그·감사 DB·상태 파일에 키 원문 없음"; fi

echo ""
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
printf "  결과: ${GREEN}PASS %d${NC} / ${RED}FAIL %d${NC}\n" "$PASS" "$FAIL"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
[ "$FAIL" -eq 0 ]
