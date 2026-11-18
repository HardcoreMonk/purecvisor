#!/usr/bin/env bash

set -uo pipefail

GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS=0; FAIL=0
pass() { echo -e "${GREEN}[PASS]${NC} $*"; PASS=$((PASS+1)); }
fail() { echo -e "${RED}[FAIL]${NC} $*"; FAIL=$((FAIL+1)); }
info() { echo -e "${CYAN}[INFO]${NC} $*"; }
expect() { if [ "$2" = "$3" ]; then pass "$1"; else fail "$1: '$2' (기대 '$3')"; fi; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
DAEMON_BIN="${PCV_CONTRACT_DAEMON_BIN:-$REPO/bin/purecvisorsd}"

. "$SCRIPT_DIR/lib/pcv_isolated_daemon.sh"

skip() { echo -e "${YELLOW}[SKIP]${NC} 화면 본문 계약 E2E: $*"; exit 0; }

for tool in bwrap python3 setsid jq sqlite3 curl node; do
    command -v "$tool" >/dev/null 2>&1 || skip "$tool 미설치"
done
[ -e "$REPO/node_modules/puppeteer" ] || skip "puppeteer 미설치(npm ci) — 이 시험은 화면이 실제로 보내는 본문이 입력이라 손으로 조립하지 않는다"
python3 -c 'import websockets' >/dev/null 2>&1 || skip "python3 websockets 모듈 미설치"
for b in /usr/sbin/zpool /usr/bin/virsh; do
    [ -e "$b" ] || skip "$b 없음 — mock bind 대상 부재(읽기 전용 루트 위에는 마운트 지점을 만들 수 없다)"
done
pcv_iso_capable || skip "$PCV_ISO_SKIP_REASON"
if [ ! -x "$DAEMON_BIN" ]; then
    info "데몬 바이너리 없음 — make daemon 시도"
    make -C "$REPO" daemon >/dev/null 2>&1 || true
fi
[ -x "$DAEMON_BIN" ] || skip "데몬 바이너리 빌드 실패 ($DAEMON_BIN)"
pcv_iso_check_bin "$DAEMON_BIN" || skip "$PCV_ISO_SKIP_REASON"

if [ "$(id -u)" = 0 ] && [ -n "${SUDO_USER:-}" ] && [ -z "${PUPPETEER_CACHE_DIR:-}" ]; then
    export PUPPETEER_CACHE_DIR
    PUPPETEER_CACHE_DIR="$(getent passwd "$SUDO_USER" | cut -d: -f6)/.cache/puppeteer"
fi

pick_free_port() {
    local p
    for p in "$@"; do
        if ! ss -ltn 2>/dev/null | grep -qE ":${p}([[:space:]]|$)"; then
            echo "$p"; return 0
        fi
    done
    return 1
}
PORT="$(pick_free_port 28341 29341 31341 33341 37341 41341)" || skip "빈 포트 확보 실패"
BASE_URL="http://127.0.0.1:$PORT/api/v1"

ADMIN_PW='Drift-E2e-Admin-not-for-prod-1'
USER_PW='Drift-E2e-User-not-for-prod-1'
VM='drift-vm'; CTR_NIC='drift-nic'; BR='drbr9'

STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-drift.XXXXXX")"
pcv_iso_check_state_fs "$STATE" || { rm -rf "$STATE"; exit 1; }
chmod 0755 "$STATE"
mkdir -p "$STATE/var-lib/lxc/$CTR_NIC" "$STATE/etc" "$STATE/var-log" "$STATE/mockbin"
SOCK="$STATE/var-lib/daemon.sock"
AUDIT_DB="$STATE/var-lib/pcv_audit.db"
DLOG="$STATE/daemon.log"
ZPOOL_LOG="$STATE/var-lib/zpool-mock.log"

printf 'lxc.uts.name = %s\nlxc.net.0.type = veth\nlxc.net.0.link = %s\n' "$CTR_NIC" "$BR" > "$STATE/var-lib/lxc/$CTR_NIC/config"

cat > "$STATE/etc/topo.xml" <<XML
<node>
  <cpu><nodes>1</nodes><sockets>1</sockets><cores>4</cores><threads>1</threads><mhz>2000</mhz></cpu>
  <domain type="test" xmlns:test="http://libvirt.org/schemas/domain/test/1.0">
    <name>${VM}</name><memory>65536</memory><vcpu>2</vcpu><os><type>hvm</type></os>
    <devices>
      <interface type="network"><source network="default"/><target dev="drtap0"/></interface>
    </devices>
    <test:runstate>1</test:runstate>
  </domain>
</node>
XML
LIBVIRT_URI='test:///etc/purecvisor/topo.xml'

cat > "$STATE/etc/daemon.conf" <<CONF
[daemon]
socket_path = /var/lib/purecvisor/daemon.sock
rest_port = $PORT
admin_user = admin
admin_password = $ADMIN_PW
jwt_secret = contract-drift-e2e-fixed-secret-not-for-prod
libvirt_uri = $LIBVIRT_URI
log_level = info
drain_timeout = 1
pool_max_conn = 2

[ws]
ping_interval_sec = 2
pong_timeout_sec = 5

[container]
rootless = false
CONF

cat > "$STATE/mockbin/zpool" <<'MOCKEOF'
#!/bin/sh
case "$1" in
    create) echo "$*" >> /var/lib/purecvisor/zpool-mock.log; exit 0 ;;
esac
exit 1
MOCKEOF

cat > "$STATE/mockbin/virsh" <<'MOCKEOF'
#!/bin/sh
if [ "$1" = domiflist ]; then
    printf ' Interface   Type      Source    Model   MAC\n-------------------------------------------\n drtap0       network   default   virtio  52:54:00:00:00:01\n'
    exit 0
fi
exit 1
MOCKEOF
chmod +x "$STATE/mockbin/"*

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
cleanup() {
    kill_daemon
    rm -rf "$STATE" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

iso_curl() { pcv_iso_exec "$(cat "$STATE/bwrap.pid" 2>/dev/null)" curl "$@"; }
uds() {
    printf '%s' "{\"jsonrpc\":\"2.0\",\"method\":\"$1\",\"params\":$2,\"id\":\"cd\"}" \
        | pcv_iso_uds_nc "$SOCK" "${3:-30}" 2>/dev/null || true
}
rpc() {
    iso_curl -s --max-time 30 -X POST -H "Authorization: Bearer $1" -H 'Content-Type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":\"cd\",\"method\":\"$2\",\"params\":$3}" "$BASE_URL/rpc" 2>/dev/null || true
}
rpc_status() {
    iso_curl -s -o /dev/null -w '%{http_code}' --max-time 30 -X POST -H "Authorization: Bearer $1" \
        -H 'Content-Type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":\"cd\",\"method\":\"$2\",\"params\":$3}" "$BASE_URL/rpc" 2>/dev/null || true
}
get_body() { iso_curl -s --max-time 30 -H "Authorization: Bearer $1" "$BASE_URL/$2" 2>/dev/null || true; }
get_status() { iso_curl -s -o /dev/null -w '%{http_code}' --max-time 30 -H "Authorization: Bearer $1" "$BASE_URL/$2" 2>/dev/null || true; }
send() {
    iso_curl -s -w '\n%{http_code}' --max-time 30 -X "$1" -H "Authorization: Bearer $2" -H 'Content-Type: application/json' \
        -d "$4" "$BASE_URL/$3" 2>/dev/null || true
}
http_of() { printf '%s' "$1" | tail -1; }
body_of() { printf '%s' "$1" | sed '$d'; }
login_token() {
    iso_curl -s --max-time 10 -X POST "$BASE_URL/auth/token" -H 'Content-Type: application/json' \
        -d "{\"username\":\"$1\",\"password\":\"$2\"}" 2>/dev/null | jq -r '.access_token // empty' 2>/dev/null || true
}
aq() { sqlite3 -cmd ".timeout 5000" "$AUDIT_DB" "$1" 2>/dev/null; }

boot() {
    setsid "$PCV_ISO_BWRAP" \
        "${PCV_ISO_BWRAP_ARGS[@]}" \
        --bind "$STATE/var-lib" /var/lib/purecvisor \
        --bind "$STATE/etc" /etc/purecvisor \
        --bind "$STATE/var-log" /var/log/purecvisor \
        --bind "$STATE" "$STATE" \
        --ro-bind "$STATE/mockbin/zpool" /usr/sbin/zpool \
        --ro-bind "$STATE/mockbin/virsh" /usr/bin/virsh \
        --setenv PCV_LIBVIRT_URI "$LIBVIRT_URI" \
        --setenv PURECVISOR_LIBVIRT_URI "$LIBVIRT_URI" \
        --chdir / \
        "$DAEMON_BIN" > "$DLOG" 2>&1 < /dev/null &
    echo "$!" > "$STATE/bwrap.pid"
    local resp
    for _ in $(seq 1 60); do
        if [ -S "$SOCK" ]; then
            resp="$(uds vm.list '{}' 10)"
            case "$resp" in
                *'"jsonrpc"'*|*'"result"'*|*'"error"'*)
                    pcv_iso_rest_ready "$(cat "$STATE/bwrap.pid" 2>/dev/null)" "$BASE_URL/health" >/dev/null \
                        && return 0 ;;
            esac
        fi
        sleep 0.5
    done
    return 1
}

screens() {
    printf '%s' "$1" > "$STATE/screens.json"
    (cd "$REPO" && timeout 120 node tests/ui/contract-drift-bodies.mjs screens "$STATE/screens.json" 2>"$STATE/screens.err")
}

echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  화면 본문·실데몬 계약 대조 격리 E2E  binary=$DAEMON_BIN${NC}"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"

if ! boot; then
    fail "S1: 격리 데몬 기동 실패"
    tail -30 "$DLOG" 2>/dev/null
    exit 1
fi
pass "S1: 격리 데몬 기동"

TOK_ADMIN="$(login_token admin "$ADMIN_PW")"
[ -n "$TOK_ADMIN" ] && pass "S2: admin 로그인" || { fail "S2: admin 로그인 실패"; exit 1; }
for spec in dviewer:viewer doperator:operator; do
    rpc "$TOK_ADMIN" auth.user.create "{\"username\":\"${spec%%:*}\",\"password\":\"$USER_PW\",\"role\":\"${spec#*:}\"}" >/dev/null
done
TOK_VIEWER="$(login_token dviewer "$USER_PW")"
TOK_OPERATOR="$(login_token doperator "$USER_PW")"
[ -n "$TOK_VIEWER" ] && [ -n "$TOK_OPERATOR" ] && pass "S3: VIEWER·OPERATOR 로그인" || { fail "S3: 역할 로그인 실패"; exit 1; }

TODAY="$(date +%F)"
AGENT_CFG="$(get_body "$TOK_ADMIN" agent/config)"
AGENT_KEY='drift-fake-key-0001'
jq -n --arg vm "$VM" --arg br "$BR" --arg d "$TODAY" --arg k "$AGENT_KEY" --argjson ac "$AGENT_CFG" '{
    agent: { key: $k, config: ($ac.data // $ac.result // $ac) },
    vm: $vm, ctr: { name: "drift-new", bridge: $br },
    pool: { mirror_name: "drmirror", stripe_name: "drstripe", disks: "/dev/drift-a /dev/drift-b" },
    audit: { username: "dviewer", method: "vm.list", from: $d, to: $d } }' > "$STATE/bodies-input.json"
BODIES="$(cd "$REPO" && timeout 180 node tests/ui/contract-drift-bodies.mjs bodies "$STATE/bodies-input.json" 2>"$STATE/bodies.err")"
if [ -z "$BODIES" ]; then
    fail "화면 본문 추출 실패 — 시정 전 UI이거나 브라우저를 찾지 못했다"
    sed 's/^/        /' "$STATE/bodies.err" | tail -15
    echo -e "${CYAN}결과: PASS=$PASS FAIL=$FAIL${NC}"
    exit 1
fi
pass "화면 조작에서 실제 본문 추출"
info "본문: $(printf '%s' "$BODIES" | jq -c 'del(.audit_search)')"
body() { printf '%s' "$BODIES" | jq -c ".$1"; }

r="$(send PUT "$TOK_ADMIN" "vms/$VM/bandwidth" "$(body bandwidth)")"
info "대역폭 응답: $(body_of "$r" | cut -c1-200)"
expect "UI 대역폭 본문 HTTP" "$(http_of "$r")" 200
expect "대역폭 inbound 반영" "$(body_of "$r" | jq -r '(.data // .result // .).inbound_kbps // empty')" 2048
expect "대역폭 인터페이스(mock domiflist)" "$(body_of "$r" | jq -r '(.data // .result // .).interface // empty')" drtap0
r="$(send PUT "$TOK_ADMIN" "vms/$VM/bandwidth" "$(body bandwidth_outbound_only)")"
expect "UI 대역폭 한쪽만 HTTP" "$(http_of "$r")" 200
r="$(send PUT "$TOK_ADMIN" "vms/$VM/bandwidth" '{"inbound_avg":1000,"outbound_avg":1000,"burst":1024}')"
expect "옛 UI 대역폭 본문은 400(판별력)" "$(http_of "$r")" 400

r="$(send PUT "$TOK_ADMIN" "vms/$VM/cpu-pin" "$(body cpu_pin)")"
expect "UI CPU 핀 본문 HTTP" "$(http_of "$r")" 200
expect "CPU 핀 응답 pinned" "$(body_of "$r" | jq -r '(.data // .result // .).pinned // empty')" true
r="$(send PUT "$TOK_ADMIN" "vms/$VM/cpu-pin" '{"vcpu_pin":"0:0"}')"
expect "옛 UI CPU 핀 본문은 400(판별력)" "$(http_of "$r")" 400

r="$(send POST "$TOK_ADMIN" storage/pools "$(body pool_mirror)")"
expect "UI 풀 생성(mirror) HTTP" "$(http_of "$r")" 200
if grep -qE ' drmirror mirror /dev/drift-a /dev/drift-b$' "$ZPOOL_LOG" 2>/dev/null; then
    pass "mirror argv가 mock zpool에 닿음"
else
    fail "mirror argv: $(cat "$ZPOOL_LOG" 2>/dev/null)"
fi
r="$(send POST "$TOK_ADMIN" storage/pools "$(body pool_stripe)")"
expect "UI 풀 생성(stripe) HTTP" "$(http_of "$r")" 200
expect "UI stripe 본문의 vdev_type은 빈 문자열(서버가 값을 그대로 zpool 인자로 넣는다)" "$(body pool_stripe | jq -r '.vdev_type')" ""
if grep -qE ' drstripe /dev/drift-a /dev/drift-b$' "$ZPOOL_LOG" 2>/dev/null; then
    pass "stripe argv에 vdev 낱말 없음"
else
    fail "stripe argv: $(cat "$ZPOOL_LOG" 2>/dev/null)"
fi

for vdev in raidz raidz1 raidz2 raidz3; do
    r="$(send POST "$TOK_ADMIN" storage/pools "{\"name\":\"dr$vdev\",\"disks\":[\"/dev/drift-a\",\"/dev/drift-b\"],\"vdev_type\":\"$vdev\"}")"
    expect "vdev_type $vdev HTTP" "$(http_of "$r")" 200
    if grep -qE " dr$vdev $vdev /dev/drift-a /dev/drift-b\$" "$ZPOOL_LOG" 2>/dev/null; then
        pass "$vdev argv가 mock zpool에 닿음"
    else
        fail "$vdev argv: $(tail -1 "$ZPOOL_LOG" 2>/dev/null)"
    fi
done
r="$(send POST "$TOK_ADMIN" storage/pools '{"name":"drabsent","disks":["/dev/drift-a"]}')"
expect "vdev_type 없으면 stripe HTTP" "$(http_of "$r")" 200
if grep -qE ' drabsent /dev/drift-a$' "$ZPOOL_LOG" 2>/dev/null; then pass "vdev_type 없을 때 argv에 vdev 낱말 없음"; else fail "vdev_type 없을 때 argv: $(tail -1 "$ZPOOL_LOG" 2>/dev/null)"; fi
ZPOOL_LINES="$(wc -l < "$ZPOOL_LOG" 2>/dev/null || echo 0)"
for bad_vdev in '"log"' '"spare"' '"cache"' '"stripe"' '"MIRROR"' '"mirror "' '"raidz4"' '"-f"' '"draid"' '5' 'true' 'null' '["mirror"]' '{"a":1}'; do
    r="$(send POST "$TOK_ADMIN" storage/pools "{\"name\":\"drbad\",\"disks\":[\"/dev/drift-a\"],\"vdev_type\":$bad_vdev}")"
    expect "허용 밖 vdev_type 400: $bad_vdev" "$(http_of "$r")" 400
    expect "허용 밖 vdev_type -32602: $bad_vdev" "$(body_of "$r" | jq -r '.error.code // empty')" -32602
done
for bad in '{"name":"drbad","disks":"/dev/drift-a","raid_level":""}' '{"name":"drbad","disks":"","raid_level":"mirror"}' \
           '{"name":"drbad","disks":{"a":1},"vdev_type":"mirror"}' '{"name":"drbad","disks":["/dev/drift-a",1],"vdev_type":""}' \
           '{"name":"drbad","disks":[],"vdev_type":""}' '{"name":"drbad","disks":[""],"vdev_type":""}'; do
    r="$(send POST "$TOK_ADMIN" storage/pools "$bad")"
    expect "잘못된 disks 400: $bad" "$(http_of "$r")" 400
    expect "잘못된 disks -32602: $bad" "$(body_of "$r" | jq -r '.error.code // empty')" -32602
done
expect "거부된 요청은 zpool을 부르지 않음" "$(wc -l < "$ZPOOL_LOG" 2>/dev/null || echo 0)" "$ZPOOL_LINES"
n="$(grep -cE 'Json-CRITICAL|json_[a-z_]+: assertion' "$DLOG")"
expect "데몬 로그에 json-glib critical 없음" "$n" 0

r="$(send POST "$TOK_ADMIN" containers "$(body container_create)")"
expect "UI 생성 본문 접수" "$(body_of "$r" | jq -r '(.data // .result // .).status // .error.message // empty')" accepted
for k in static_ip gateway dns; do
    expect "본문에 $k 없음" "$(body container_create | jq -r "has(\"$k\")")" false
done
LIST="$(get_body "$TOK_ADMIN" containers)"
NICS="$(get_body "$TOK_ADMIN" "containers/$CTR_NIC/nics")"
expect "실제 NIC 응답의 브리지" "$(printf '%s' "$NICS" | jq -r '(.data // .result // .)[0].bridge // empty')" "$BR"

CTR_CFG="$STATE/var-lib/lxc/$CTR_NIC/config"
r="$(send PUT "$TOK_ADMIN" "containers/$CTR_NIC/limits" "$(body limits_cpu)")"
expect "자원 제한 UI CPU 본문 HTTP" "$(http_of "$r")" 200
expect "자원 제한 적용 시점" "$(body_of "$r" | jq -r '(.data // .result // .).applied // empty')" config
grep -q '^lxc.cgroup2.cpu.max = 50000 100000' "$CTR_CFG" && pass "자원 제한 cpu_percent가 config에 남음" || fail "자원 제한 cpu.max: $(grep cgroup2 "$CTR_CFG" | tr '\n' ';')"
grep -q '^lxc.cgroup2.cpu.weight = 200' "$CTR_CFG" && pass "자원 제한 cpu_weight가 config에 남음" || fail "자원 제한 cpu.weight: $(grep cgroup2 "$CTR_CFG" | tr '\n' ';')"
r="$(send PUT "$TOK_ADMIN" "containers/$CTR_NIC/limits" "$(body limits_mem)")"
expect "자원 제한 UI 메모리 본문 HTTP" "$(http_of "$r")" 200
grep -q '^lxc.cgroup2.memory.max = 268435456' "$CTR_CFG" && pass "자원 제한 memory_mb가 config에 남음" || fail "자원 제한 memory.max: $(grep cgroup2 "$CTR_CFG" | tr '\n' ';')"
for old_body in '{"cpu_shares":1024,"cpu_quota":100000}' '{"memory_limit_mb":512,"swap_limit_mb":0}'; do
    expect "옛 UI 자원 제한 본문은 400(판별력): $old_body" "$(http_of "$(send PUT "$TOK_ADMIN" "containers/$CTR_NIC/limits" "$old_body")")" 400
done

expect "게스트 실행 본문에 args 없음" "$(body guest_exec | jq -r 'has("args")')" false
r="$(send POST "$TOK_ADMIN" "vms/$VM/guest-exec" "$(body guest_exec)")"
info "게스트 실행 응답: $(body_of "$r" | cut -c1-160)"
[ "$(body_of "$r" | jq -r '.error.code // empty')" != "-32602" ] && [ "$(http_of "$r")" != 400 ] && pass "게스트 실행 UI 본문은 입력 검증을 지난다" || fail "게스트 실행 HTTP $(http_of "$r")"
expect "게스트 실행 args만 있으면 400(서버는 command만 읽는다)" "$(http_of "$(send POST "$TOK_ADMIN" "vms/$VM/guest-exec" '{"args":["/etc/hostname"]}')")" 400

S="$(screens "$(jq -n --arg c "$CTR_NIC" --argjson l "$LIST" --argjson n "$NICS" '{ctr:$c, containers:$l, nics:$n}')")"
expect "요약 탭 Bridge는 실제 값" "$(printf '%s' "$S" | jq -r '.ctr_bridge')" "$BR"
expect "요약 탭 AppArmor는 알 수 없음" "$(printf '%s' "$S" | jq -r '.ctr_apparmor')" '알 수 없음'
expect "옵션 탭 Unprivileged는 실제 값" "$(printf '%s' "$S" | jq -r '.ctr_opt_unprivileged')" \
    "$(printf '%s' "$LIST" | jq -r --arg c "$CTR_NIC" '(.data // .result // .)[] | select(.name==$c) | if .privileged then "No" else "Yes" end')"

agent_enabled() { get_body "$TOK_ADMIN" agent/config | jq -r '(.data // .result // .).providers[] | select(.name=="Claude") | .enabled'; }
expect "AI Agent 저장 전 Claude 꺼짐" "$(agent_enabled)" false
expect "AI Agent UI 본문에 enabled 없음" "$(body agent_enable | jq -r '[.providers[] | has("enabled")] | any')" false
expect "AI Agent UI 저장(키 넣기) HTTP" "$(http_of "$(send PUT "$TOK_ADMIN" agent/config "$(body agent_enable)")")" 200
expect "AI Agent 키를 넣으면 켜짐" "$(agent_enabled)" true
if get_body "$TOK_ADMIN" agent/config | grep -qF "$AGENT_KEY"; then fail "AI Agent 조회에 키 원문"; else pass "AI Agent 조회는 키를 가린다"; fi
expect "AI Agent enabled:false만 보내도 그대로(서버는 읽지 않는다)" \
    "$(send PUT "$TOK_ADMIN" agent/config '{"providers":[{"name":"Claude","enabled":false}]}' >/dev/null; agent_enabled)" true

agent_field() { get_body "$TOK_ADMIN" agent/config | jq -r --arg n "$1" --arg f "$2" '(.data // .result // .).providers[] | select(.name==$n) | .[$f]'; }
MASKED_LONG="$(agent_field Claude api_key)"
expect "AI Agent 긴 키의 가림값은 앞뒤 힌트" "$MASKED_LONG" 'dri...001'
expect "AI Agent *** 저장 HTTP" "$(http_of "$(send PUT "$TOK_ADMIN" agent/config '{"providers":[{"name":"Claude","api_key":"***"}]}')")" 200
expect "AI Agent ***를 돌려보내도 키가 덮이지 않는다" "$(agent_field Claude api_key)" "$MASKED_LONG"
send PUT "$TOK_ADMIN" agent/config '{"providers":[{"name":"Claude","api_key":"***","model":"drift-model-2"}]}' >/dev/null
expect "AI Agent *** 옆의 model은 반영" "$(agent_field Claude model)" drift-model-2
expect "AI Agent *** 옆의 model을 바꿔도 키 유지" "$(agent_field Claude api_key)" "$MASKED_LONG"
send PUT "$TOK_ADMIN" agent/config '{"providers":[{"name":"Claude","api_key":"dri...001"}]}' >/dev/null
expect "AI Agent ...가림값을 돌려보내도 키 유지(기존 경로)" "$(agent_field Claude api_key)" "$MASKED_LONG"

SHORT_KEY='drift-s1'
send PUT "$TOK_ADMIN" agent/config "{\"providers\":[{\"name\":\"Gemini\",\"api_key\":\"$SHORT_KEY\"}]}" >/dev/null
expect "AI Agent 짧은 키의 가림값은 ***" "$(agent_field Gemini api_key)" '***'
ROUNDTRIP="$(get_body "$TOK_ADMIN" agent/config | jq -c '{providers:[(.data // .result // .).providers[] | {name,model,api_key,endpoint}]}')"
expect "AI Agent 조회 응답을 그대로 저장 HTTP" "$(http_of "$(send PUT "$TOK_ADMIN" agent/config "$ROUNDTRIP")")" 200
expect "AI Agent 왕복 뒤 짧은 키 공급자 켜짐" "$(agent_field Gemini enabled)" true
expect "AI Agent 왕복 뒤 긴 키 유지" "$(agent_field Claude api_key)" "$MASKED_LONG"
for secret in "$AGENT_KEY" "$SHORT_KEY"; do
    if grep -qF "$secret" "$DLOG"; then fail "AI Agent 데몬 로그에 키 원문"; else pass "AI Agent 데몬 로그에 키 원문 없음"; fi
done
send PUT "$TOK_ADMIN" agent/config '{"providers":[{"name":"Gemini","api_key":""}]}' >/dev/null
expect "AI Agent UI 저장(키 비움) HTTP" "$(http_of "$(send PUT "$TOK_ADMIN" agent/config "$(body agent_disable)")")" 200
expect "AI Agent 키를 비우면 꺼짐" "$(agent_enabled)" false
for old_body in '{"rate_limit_sec":5,"timeout_sec":5}' '{"rate_limit_sec":5}'; do
    expect "옛 설정 탭 본문은 400(판별력): $old_body" "$(http_of "$(send PUT "$TOK_ADMIN" agent/config "$old_body")")" 400
done
S="$(screens "$(jq -n --argjson a "$(get_body "$TOK_ADMIN" agent/config)" '{agent:$a}')")"
expect "AI Agent 설정 탭 입력 없음" "$(printf '%s' "$S" | jq -r '.agent_settings_inputs')" 0
expect "AI Agent 설정 탭 저장 버튼 없음" "$(printf '%s' "$S" | jq -r '.agent_settings_save')" false
if printf '%s' "$S" | jq -r '.agent_settings_text' | grep -q 300; then pass "AI Agent 설정 탭이 서버의 호출 간격을 보여 준다"; else fail "AI Agent 설정 탭: $(printf '%s' "$S" | jq -r '.agent_settings_text' | cut -c1-120)"; fi

for spec in "VIEWER:$TOK_VIEWER" "OPERATOR:$TOK_OPERATOR" "ADMIN:$TOK_ADMIN"; do
    who="${spec%%:*}"; tok="${spec#*:}"
    r="$(rpc "$tok" healing.status '{}')"
    expect "$who healing.status mode" "$(printf '%s' "$r" | jq -r '(.data // .result).mode // .error.message // empty')" dry_run
    expect "$who healing.status pending 정수" "$(printf '%s' "$r" | jq -r '(.data // .result).pending | type')" number
done
expect "키는 mode·pending뿐" "$(rpc "$TOK_VIEWER" healing.status '{}' | jq -c '(.data // .result) | keys')" '["mode","pending"]'
expect "무인증 401" "$(iso_curl -s -o /dev/null -w '%{http_code}' --max-time 10 -X POST -H 'Content-Type: application/json' \
    -d '{"jsonrpc":"2.0","id":"cd","method":"healing.status","params":{}}' "$BASE_URL/rpc" 2>/dev/null)" 401
expect "healing.pending 응답은 배열 그대로(ADMIN)" "$(rpc "$TOK_ADMIN" healing.pending '{}' | jq -r '(.data // .result) | type')" array
expect "pending 수 = 목록 길이(ADMIN)" "$(rpc "$TOK_ADMIN" healing.status '{}' | jq -r '(.data // .result).pending')" \
    "$(rpc "$TOK_ADMIN" healing.pending '{}' | jq -r '(.data // .result) | length')"

expect "VIEWER healing.pending 빈 배열" "$(rpc "$TOK_VIEWER" healing.pending '{}' | jq -c '(.data // .result)')" '[]'
expect "VIEWER healing.status.pending 집계 그대로" "$(rpc "$TOK_VIEWER" healing.status '{}' | jq -r '(.data // .result).pending')" \
    "$(rpc "$TOK_ADMIN" healing.status '{}' | jq -r '(.data // .result).pending')"
expect "VIEWER set_mode 403" "$(rpc_status "$TOK_VIEWER" healing.set_mode '{"mode":"active"}')" 403
expect "거부 뒤 모드 그대로" "$(rpc "$TOK_VIEWER" healing.status '{}' | jq -r '(.data // .result).mode // empty')" dry_run

draw_healing() {
    jq -n --argjson st "$(rpc "$TOK_VIEWER" healing.status '{}')" --argjson pe "$(rpc "$TOK_VIEWER" healing.pending '{}')" \
        --argjson hi "$(rpc "$TOK_VIEWER" healing.history '{}')" --argjson ag "$(rpc "$TOK_VIEWER" agent.history '{}')" \
        '{rpc:{"healing.status":$st,"healing.pending":$pe,"healing.history":$hi,"agent.history":$ag}, role:"VIEWER"}'
}
S="$(screens "$(draw_healing)")"
for k in selfhealing_badge mobile_mode overview_mode; do
    expect "dry_run 화면 $k" "$(printf '%s' "$S" | jq -r ".$k")" 'DRY RUN'
done
expect "개요 대기 패널" "$(printf '%s' "$S" | jq -r '.overview_pending')" '대기 중인 액션 없음'
r="$(rpc "$TOK_ADMIN" healing.set_mode '{"mode":"active"}')"
expect "ADMIN set_mode active" "$(printf '%s' "$r" | jq -r '(.data // .result).mode // empty')" active
expect "전환 뒤 VIEWER가 active를 읽음" "$(rpc "$TOK_VIEWER" healing.status '{}' | jq -r '(.data // .result).mode // empty')" active
S="$(screens "$(draw_healing)")"
for k in selfhealing_badge mobile_mode overview_mode; do
    expect "active 화면 $k" "$(printf '%s' "$S" | jq -r ".$k")" ACTIVE
done
rpc "$TOK_ADMIN" healing.set_mode '{"mode":"dry_run"}' >/dev/null

for _ in 1 2 3; do get_status "$TOK_VIEWER" vms >/dev/null; get_status "$TOK_OPERATOR" vms >/dev/null; done
sleep 1
Q="$(printf '%s' "$BODIES" | jq -r '.audit_search')"
info "UI 쿼리: $Q"
expected="$(aq "SELECT COUNT(*) FROM audit_log WHERE username='dviewer' AND method LIKE 'vm.list';")"
[ "${expected:-0}" -ge 3 ] && pass "전제: dviewer vm.list 감사 행 $expected" || fail "전제: dviewer vm.list 감사 행 ${expected:-?}"
b="$(get_body "$TOK_ADMIN" "audit/search$Q")"
expect "UI 쿼리 행 수 = DB" "$(printf '%s' "$b" | jq -r '(.data // .result // .).items | length')" "$expected"
expect "UI 쿼리 결과는 그 사용자만" "$(printf '%s' "$b" | jq -r '[(.data // .result // .).items[] | select(.username!="dviewer" or .method!="vm.list")] | length')" 0
all="$(get_body "$TOK_ADMIN" 'audit/search?limit=500')"
n="$(printf '%s' "$all" | jq -r '[(.data // .result // .).items[] | select(.username!="dviewer")] | length')"
[ "${n:-0}" -gt 0 ] && pass "판별력: 필터 없으면 다른 사용자 행도 나온다($n)" || fail "판별력: 다른 사용자 행 ${n:-?}"
expect "미래 from_ts는 0행" "$(get_body "$TOK_ADMIN" 'audit/search?from_ts=2999-01-01%2000:00:00' | jq -r '(.data // .result // .).items | length')" 0
expect "과거 to_ts는 0행" "$(get_body "$TOK_ADMIN" 'audit/search?to_ts=2000-01-01+00:00:00' | jq -r '(.data // .result // .).items | length')" 0
ids() { printf '%s' "$1" | jq -c '[(.data // .result // .).items[].id]'; }
page3="$(ids "$(get_body "$TOK_ADMIN" 'audit/search?username=dviewer&limit=3')")"
page2="$(ids "$(get_body "$TOK_ADMIN" 'audit/search?username=dviewer&limit=2&offset=1')")"
expect "limit·offset 페이지가 이어진다" "$page2" "$(printf '%s' "$page3" | jq -c '.[1:3]')"

FILTER='username=dviewer&method=vm.list'
page_field() { get_body "$TOK_ADMIN" "audit/search?$FILTER&$1" | jq -c "(.data // .result // .) | $2"; }
expect "has_more: 딱 맞으면 false" "$(page_field "limit=$expected" .has_more)" false
expect "has_more: 한 행 남으면 true" "$(page_field "limit=$((expected - 1))" .has_more)" true
expect "has_more: 한 행 더 읽어도 items는 limit만큼" "$(page_field "limit=$((expected - 1))" '.items | length')" "$((expected - 1))"
expect "has_more: 넉넉하면 false" "$(page_field "limit=$((expected + 1))" .has_more)" false
expect "has_more: offset까지 더해 딱 맞으면 false" "$(page_field "limit=$((expected - 1))&offset=1" .has_more)" false
expect "has_more: offset 뒤 한 행 남으면 true" "$(page_field "limit=$((expected - 2))&offset=1" .has_more)" true
expect "total은 읽은 행 수(offset+limit+1 상한)" "$(page_field "limit=1&offset=1" .total)" 3
expect "total은 일치 행이 모자라면 그 수" "$(page_field "limit=$((expected + 5))" .total)" "$expected"
expect "끝을 넘은 offset은 빈 items·has_more false" "$(page_field "limit=2&offset=$((expected + 3))" '[(.items | length), .has_more]')" '[0,false]'
expect "쿼리 없는 GET 200" "$(get_status "$TOK_ADMIN" audit/search)" 200
expect "올바른 시각 경계는 200" "$(get_status "$TOK_ADMIN" 'audit/search?from_ts=2026-01-01%2023:59:59')" 200
for q in 'limit=abc' 'limit=-1' 'limit=0' 'limit=99999999999' 'offset=x' 'offset=-1' 'from_ts=abc' 'from_ts=2026-13-01%2000:00:00' 'to_ts=2026-11-01' \
         'from_ts=2026-01-%201%2000:00:00' 'from_ts=2026-%201-01%2000:00:00' 'from_ts=2026-01-01%20-0:00:00' 'to_ts=2026-01-01%2000:-0:00' 'to_ts=2026-01-01%2000:00:-1' \
         'from_ts=%202026-01-01%200:00:00' 'from_ts=2026-01-01%20%200:00:00' 'from_ts=2026:01:01%2000:00:00' \
         'user=bob' 'action=vm.list' 'from=2026-11-01' 'to=2026-11-01' 'limit=1&limit=2' 'username=' 'method=' 'nokey'; do
    expect "잘못된 쿼리 400: $q" "$(get_status "$TOK_ADMIN" "audit/search?$q")" 400
done
for spec in "VIEWER:$TOK_VIEWER" "OPERATOR:$TOK_OPERATOR"; do
    who="${spec%%:*}"; tok="${spec#*:}"
    expect "$who 정상 쿼리 403" "$(get_status "$tok" 'audit/search?limit=5')" 403
    expect "$who 잘못된 쿼리도 403(입력 판정 앞)" "$(get_status "$tok" 'audit/search?limit=abc')" 403
done
denied="$(get_body "$TOK_VIEWER" 'audit/search?limit=5')"
S="$(screens "$(jq -n --argjson a "$denied" '{audit:$a, audit_status:403, role:"VIEWER"}')")"
expect "실제 403은 화면에서 오류" "$(printf '%s' "$S" | jq -r '.audit_error')" true
if printf '%s' "$S" | jq -r '.audit_text' | grep -q '검색 결과 없음'; then fail "403이 결과 없음으로 보인다"; else pass "403이 결과 없음으로 보이지 않는다"; fi
S="$(screens "$(jq -n --argjson a "$b" '{audit:$a}')")"
expect "실제 응답의 행을 화면이 그린다" "$(printf '%s' "$S" | jq -r '.audit_rows | length')" "$expected"
expect "그린 행의 사용자" "$(printf '%s' "$S" | jq -r '.audit_rows[0][1]')" dviewer

PONG="$(printf '%s' "$BODIES" | jq -r '.pong[0] // empty')"
expect "UI가 ping에 보내는 프레임" "$PONG" '{"type":"pong"}'
printf '%s' "$TOK_VIEWER" > "$STATE/ws.token"
printf '%s' "$PONG" > "$STATE/ws.pong"
cat > "$STATE/ws_client.py" <<'PYEOF'
import json, sys, time
from websockets.sync.client import connect
from websockets.exceptions import ConnectionClosed
url, token_file, pong_file, reply_mode, wait, out = sys.argv[1:7]
token = open(token_file).read().strip()
pong = open(pong_file).read()
reply = {"answer": pong, "none": None, "other": '{"type":"noop"}'}[reply_mode]
pings = 0
started = time.monotonic()
def note(state, detail=""):
    with open(out, "w") as f:
        json.dump({"state": state, "detail": detail, "pings": pings, "seconds": round(time.monotonic() - started, 1)}, f)
try:
    with connect(url, open_timeout=8, close_timeout=3) as ws:
        ws.send(json.dumps({"type": "auth", "token": token}))
        deadline = time.monotonic() + float(wait)
        while time.monotonic() < deadline:
            try:
                msg = ws.recv(timeout=max(0.1, deadline - time.monotonic()))
            except TimeoutError:
                break
            if '"ping"' in str(msg):
                pings += 1
                if reply is not None:
                    ws.send(reply)
        note("open")
except ConnectionClosed as e:
    rcvd = getattr(e, "rcvd", None)
    note("closed", "%s:%s" % (getattr(rcvd, "code", ""), getattr(rcvd, "reason", "")))
except Exception as e:
    note("error", type(e).__name__)
PYEOF
BP="$(cat "$STATE/bwrap.pid")"
for mode in answer none other; do
    pcv_iso_exec "$BP" python3 "$STATE/ws_client.py" "ws://127.0.0.1:$PORT/api/v1/ws/events" "$STATE/ws.token" "$STATE/ws.pong" \
        "$mode" 16 "$STATE/ws-$mode.out" >/dev/null 2>&1 &
done
wait_ws() {
    local end=$((SECONDS + 30))
    while [ "$SECONDS" -lt "$end" ]; do
        [ -s "$STATE/ws-answer.out" ] && [ -s "$STATE/ws-none.out" ] && [ -s "$STATE/ws-other.out" ] && return 0
        sleep 0.5
    done
    return 1
}
wait_ws || fail "WS 클라이언트가 시한 안에 끝나지 않음"
info "pong 답함: $(cat "$STATE/ws-answer.out" 2>/dev/null)"
info "답 없음: $(cat "$STATE/ws-none.out" 2>/dev/null)"
info "다른 프레임: $(cat "$STATE/ws-other.out" 2>/dev/null)"
expect "pong으로 답한 연결은 시한(5초)을 넘겨도 열려 있음" "$(jq -r '.state' "$STATE/ws-answer.out" 2>/dev/null)" open
n="$(jq -r '.pings' "$STATE/ws-answer.out" 2>/dev/null)"
[ "${n:-0}" -ge 4 ] && pass "살아 있는 동안 ping을 계속 받음($n)" || fail "받은 ping ${n:-?}"
expect "답 없는 연결은 닫힘" "$(jq -r '.state' "$STATE/ws-none.out" 2>/dev/null)" closed
expect "답 없는 연결의 닫힘 사유" "$(jq -r '.detail' "$STATE/ws-none.out" 2>/dev/null)" '1001:pong timeout'
expect "pong이 아닌 프레임만 보낸 연결도 닫힘(반사실)" "$(jq -r '.detail' "$STATE/ws-other.out" 2>/dev/null)" '1001:pong timeout'

echo ""
echo -e "${CYAN}결과: PASS=$PASS FAIL=$FAIL${NC}"
[ "$FAIL" -eq 0 ]
