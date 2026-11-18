#!/usr/bin/env bash

set -uo pipefail

GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS=0; FAIL=0
pass() { echo -e "${GREEN}[PASS]${NC} $*"; PASS=$((PASS+1)); }
fail() { echo -e "${RED}[FAIL]${NC} $*"; FAIL=$((FAIL+1)); }
info() { echo -e "${CYAN}[INFO]${NC} $*"; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"

DAEMON_BIN="${PCV_DAEMON_BIN:-$REPO/bin/purecvisorsd}"
PCVCTL_BIN="$REPO/bin/pcvctl"

. "$SCRIPT_DIR/lib/pcv_isolated_daemon.sh"

skip() { echo -e "${YELLOW}[SKIP]${NC} OPERATOR 호스트 자원 참조 경계 E2E: $*"; exit 0; }

command -v bwrap    >/dev/null 2>&1 || skip "bwrap(bubblewrap) 미설치"
command -v sqlite3  >/dev/null 2>&1 || skip "sqlite3 미설치"
command -v curl     >/dev/null 2>&1 || skip "curl 미설치"
command -v python3  >/dev/null 2>&1 || skip "python3 미설치"
command -v jq       >/dev/null 2>&1 || skip "jq 미설치"
command -v setsid   >/dev/null 2>&1 || skip "setsid 미설치"
pcv_iso_capable || skip "$PCV_ISO_SKIP_REASON"
if [ ! -x "$DAEMON_BIN" ] || [ ! -x "$PCVCTL_BIN" ]; then
    info "데몬·pcvctl 바이너리 없음 — make daemon cli 시도"
    make -C "$REPO" daemon cli >/dev/null 2>&1 || true
fi
[ -x "$DAEMON_BIN" ] || skip "데몬 바이너리 빌드 실패 ($DAEMON_BIN)"
[ -x "$PCVCTL_BIN" ] || skip "pcvctl 바이너리 빌드 실패 ($PCVCTL_BIN)"
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
PORT="$(pick_free_port 28217 29217 31217 33217 37217 41217)" || skip "빈 포트 확보 실패"
BASE_URL="http://127.0.0.1:$PORT/api/v1"

ADMIN_PW='OrbResBoundAdminPw-not-for-prod'
ALICE_PW='OrbAliceE2ePw-not-for-prod1'
BOB_PW='OrbBobE2ePw-not-for-prod1'
NOB_PW='OrbNobE2ePw-not-for-prod1'
VIC_PW='OrbVicE2ePw-not-for-prod1'

STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-a02res.XXXXXX")"
pcv_iso_check_state_fs "$STATE" || { rm -rf "$STATE"; exit 1; }
mkdir -p "$STATE/var-lib" "$STATE/etc" "$STATE/iso" "$STATE/other" "$STATE/disks" "$STATE/def" "$STATE/elsewhere"
SOCK="$STATE/var-lib/daemon.sock"
AUDIT_DB="$STATE/var-lib/pcv_audit.db"
DLOG="$STATE/daemon.log"
for f in a legacy unreg; do printf 'Orb-%s\n' "$f" > "$STATE/iso/$f.iso"; done
A_ISO="$STATE/iso/a.iso"; LEGACY_ISO="$STATE/iso/legacy.iso"; UNREG_ISO="$STATE/iso/unreg.iso"
NOPE_ISO="$STATE/iso/nope.iso"

owner_md() { printf '<metadata><pcv:owner xmlns:pcv="urn:purecvisor:metadata">%s</pcv:owner></metadata>' "$1"; }
cat > "$STATE/etc/topo.xml" <<XML
<node>
  <domain type="test">
    <name>a02-a</name><memory>65536</memory><os><type>hvm</type></os>
    $(owner_md alice)
    <devices>
      <disk type="file" device="cdrom"><driver name="qemu" type="raw"/><target dev="sda" bus="sata"/><readonly/></disk>
      <disk type="file" device="cdrom"><driver name="qemu" type="raw"/><source file="${LEGACY_ISO}"/><target dev="sdb" bus="sata"/><readonly/></disk>
      <interface type="bridge"><source bridge="br-legacy"/></interface>
    </devices>
  </domain>
  <domain type="test">
    <name>a02-b</name><memory>65536</memory><os><type>hvm</type></os>
    $(owner_md bob)
    <devices>
      <disk type="file" device="cdrom"><driver name="qemu" type="raw"/><target dev="sda" bus="sata"/><readonly/></disk>
    </devices>
  </domain>
  <domain type="test">
    <name>a02-c</name><memory>65536</memory><os><type>hvm</type></os>
    $(owner_md nob)
    <devices>
      <disk type="file" device="cdrom"><driver name="qemu" type="raw"/><target dev="sda" bus="sata"/><readonly/></disk>
    </devices>
  </domain>
  <domain type="test">
    <name>a02-n</name><memory>65536</memory><os><type>hvm</type></os>
    <devices>
      <hostdev mode="subsystem" type="usb"><source><vendor id="0x1d6b"/><product id="0x0002"/></source></hostdev>
    </devices>
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
jwt_secret = a02-resbound-e2e-fixed-secret-not-for-prod-01
libvirt_uri = $LIBVIRT_URI
log_level = info
drain_timeout = 1
pool_max_conn = 2

[storage]
image_dir = $STATE/def
iso_dirs = $STATE/iso

[network]
default_bridge = pcvnat0
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
cleanup() {
    kill_daemon
    rm -rf "$STATE" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

iso_curl() { pcv_iso_exec "$(cat "$STATE/bwrap.pid" 2>/dev/null)" curl "$@"; }
uds() {
    printf '%s' "{\"jsonrpc\":\"2.0\",\"method\":\"$1\",\"params\":$2,\"id\":\"a02\"}" \
        | pcv_iso_uds_nc "$SOCK" "${3:-30}" 2>/dev/null || true
}
rpc() {
    iso_curl -s --max-time 30 -X POST -H "Authorization: Bearer $1" -H 'Content-Type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":\"a02\",\"method\":\"$2\",\"params\":$3}" "$BASE_URL/rpc" 2>/dev/null || true
}
login_token() {
    iso_curl -s --max-time 6 -X POST "$BASE_URL/auth/token" -H 'Content-Type: application/json' \
        -d "{\"username\":\"$1\",\"password\":\"$2\"}" 2>/dev/null \
        | jq -r '.access_token // empty' 2>/dev/null || true
}
pcvctl_iso() {
    [ -S "$SOCK" ] || return 125
    "$PCVCTL_BIN" --socket="$SOCK" "$@"
}

ecode() { printf '%s' "$1" | jq -r '.error.code // empty' 2>/dev/null; }
emsg() { printf '%s' "$1" | jq -r '.error.message // empty' 2>/dev/null; }
is_rpc() { printf '%s' "$1" | jq -e 'type=="object" and (has("result") or has("data") or has("error"))' >/dev/null 2>&1; }
is_ok() { printf '%s' "$1" | jq -e 'type=="object" and (has("result") or has("data")) and (.error == null)' >/dev/null 2>&1; }
res() {
    printf '%s' "$1" | jq -c "(if has(\"result\") then .result else .data end) | $2" 2>/dev/null
}
brief() {
    printf '%s' "$1" | jq -c 'if type=="object" and .error then {error:{code:.error.code,message:.error.message}}
                              elif type=="object" and has("data") then {data:.data}
                              elif type=="object" then {result:.result} else . end' 2>/dev/null | head -c 240
}

MSG_NO_TENANT_PREFIX='Permission denied: this account has no tenant'
msg_not_allowed() { printf 'Permission denied: %s is not a resource allowed for this tenant' "$1"; }

expect_deny() {
    local code msg
    code="$(ecode "$2")"; msg="$(emsg "$2")"
    if [ "$3" = no_tenant ]; then
        if [ "$code" = "-32006" ] && [[ "$msg" == "$MSG_NO_TENANT_PREFIX"* ]]; then
            pass "$1: −32006 NO_TENANT 문구"; return 0
        fi
    elif [ "$code" = "-32006" ] && [ "$msg" = "$(msg_not_allowed "$4")" ]; then
        pass "$1: −32006 NOT_ALLOWED 문구(파라미터 $4)"; return 0
    fi
    fail "$1: 기대 −32006 ${3}${4:+($4)} — 응답 $(brief "$2")"
    return 1
}

expect_not_deny() {
    if is_rpc "$2" && [ "$(ecode "$2")" != "-32006" ]; then
        pass "$1: −32006 아님 — $(brief "$2")"
    else
        fail "$1: 기대 −32006 아님 — 응답 ${2:+$(brief "$2")}${2:-<empty>}"
    fi
}

aq() { sqlite3 -cmd ".timeout 5000" "$AUDIT_DB" "$1" 2>/dev/null; }
max_id() { local v; v="$(aq 'SELECT COALESCE(MAX(id),0) FROM audit_log;')"; echo "${v:-0}"; }
dump_rows() {
    aq "SELECT id||'|'||IFNULL(username,'NULL')||'|'||method||'|'||IFNULL(target,'')||'|'||result||'|'||IFNULL(error_code,'')||'|'||IFNULL(src_ip,'') FROM audit_log WHERE $1 ORDER BY id;" \
        | sed 's/^/        /'
}

audit_denied() {
    local w="id > $2 AND method='$3' AND error_code=-32006" n=0 u=0
    for _ in $(seq 1 20); do
        n="$(aq "SELECT COUNT(*) FROM audit_log WHERE $w;")"
        [ "${n:-0}" -ge 1 ] && break
        sleep 0.25
    done
    sleep 0.3
    n="$(aq "SELECT COUNT(*) FROM audit_log WHERE $w;")"
    u="$(aq "SELECT COUNT(*) FROM audit_log WHERE $w AND result='denied' AND username='$4';")"
    if [ "$n" = "1" ] && [ "$u" = "1" ]; then
        pass "6($1): $3 거절 감사 정확히 1행(denied/−32006, username=$4, $(aq "SELECT target FROM audit_log WHERE $w;"))"
    else
        fail "6($1): $3 −32006 행 ${n:-?}(기대 1), denied·username=$4 행 ${u:-?}"
        dump_rows "id > $2 AND method='$3'"
    fi
}

boot() {
    setsid "$PCV_ISO_BWRAP" \
        "${PCV_ISO_BWRAP_ARGS[@]}" \
        --bind "$STATE/var-lib" /var/lib/purecvisor \
        --bind "$STATE/etc" /etc/purecvisor \
        --bind "$STATE" "$STATE" \
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

echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  OPERATOR 호스트 자원 참조 경계 격리 E2E (operator-resource-boundary)${NC}"
echo -e "${CYAN}  binary=$DAEMON_BIN${NC}"
echo -e "${CYAN}  port=$PORT  state=$STATE  sock=$SOCK${NC}"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"

if ! boot; then
    fail "S1: 격리 데몬 기동 실패(UDS·REST /health 준비 안 됨)"
    tail -30 "$DLOG" 2>/dev/null
    exit 1
fi
pass "S1: 격리 데몬 기동 + UDS 응답 + REST /health 준비"
BASE="$(max_id)"
info "BASE(audit_log MAX(id)) = $BASE"

mk_user() {
    local p="{\"username\":\"$1\",\"password\":\"$2\",\"role\":\"$3\"${4:+,\"tenant\":\"$4\"}}" r
    r="$(uds auth.user.create "$p")"
    if is_ok "$r"; then
        pass "S2: UDS auth.user.create $1($3${4:+, tenant $4})"
    else
        fail "S2: UDS auth.user.create $1 실패 — $(brief "$r")"
    fi
}
mk_user alice "$ALICE_PW" operator ta
mk_user bob "$BOB_PW" operator tb
mk_user nob "$NOB_PW" operator
mk_user vic "$VIC_PW" viewer ta
TOK_ALICE="$(login_token alice "$ALICE_PW")"
TOK_BOB="$(login_token bob "$BOB_PW")"
TOK_NOB="$(login_token nob "$NOB_PW")"
TOK_VIC="$(login_token vic "$VIC_PW")"
if [ -n "$TOK_ALICE" ] && [ -n "$TOK_BOB" ] && [ -n "$TOK_NOB" ] && [ -n "$TOK_VIC" ]; then
    pass "S3: REST 로그인 alice·bob·nob·vic → access token"
else
    fail "S3: REST 로그인 실패(alice ${#TOK_ALICE}, bob ${#TOK_BOB}, nob ${#TOK_NOB}, vic ${#TOK_VIC})"
    tail -20 "$DLOG" 2>/dev/null
    exit 1
fi

VMC='"vcpu":1,"memory_mb":512,"disk_size_gb":1,"qos_min_mbps":10,"qos_max_mbps":100'

r="$(uds vm.mount_iso "{\"vm_id\":\"a02-n\",\"iso_path\":\"$UNREG_ISO\"}")"
expect_not_deny "1: UDS(ADMIN) vm.mount_iso a02-n 미등록 unreg.iso" "$r"
r="$(uds vm.create "{\"name\":\"a02-adm\",\"iso_path\":\"$UNREG_ISO\",\"storage_type\":\"qcow2\",\"image_dir\":\"$STATE/elsewhere\",$VMC}")"
expect_not_deny "1: UDS(ADMIN) vm.create 미등록 ISO + image_dir=\$STATE/elsewhere" "$r"

ok2=1
for call in "resource.register|{\"kind\":\"iso\",\"name\":\"iso-a\",\"ref\":\"$A_ISO\"}" \
            "resource.grant|{\"name\":\"iso-a\",\"tenant\":\"ta\"}" \
            "resource.register|{\"kind\":\"storage\",\"name\":\"st-a\",\"ref\":\"$STATE/disks\"}" \
            "resource.grant|{\"name\":\"st-a\",\"tenant\":\"ta\"}"; do
    r="$(uds "${call%%|*}" "${call#*|}")"
    is_ok "$r" || { ok2=0; info "2: ${call%%|*} 실패 — $(brief "$r")"; }
done
r="$(uds resource.list '{}')"
info "2: resource.list → $(brief "$r")"
n2="$(printf '%s' "$r" | jq -r --arg a "$A_ISO" --arg d "$STATE/disks" '
    [.result[]? | select((.name=="iso-a" and .kind=="iso" and .ref==$a and .tenants==["ta"])
                      or (.name=="st-a" and .kind=="storage" and .ref==$d and .tenants==["ta"]))] | length' 2>/dev/null)"
len2="$(printf '%s' "$r" | jq -r '.result | length' 2>/dev/null)"
if [ "$ok2" = 1 ] && [ "$n2" = "2" ] && [ "$len2" = "2" ]; then
    pass "2: register·grant iso-a·st-a ok, resource.list 2항목·tenants [\"ta\"]"
else
    fail "2: register·grant ok=$ok2, list 일치 ${n2:-?}/2, 길이 ${len2:-?}"
fi

r="$(rpc "$TOK_ALICE" vm.mount_iso "{\"vm_id\":\"a02-a\",\"iso_path\":\"$A_ISO\"}")"
expect_not_deny "3: alice vm.mount_iso a02-a a.iso(허용)" "$r"
M="$(max_id)"
r="$(rpc "$TOK_BOB" vm.mount_iso "{\"vm_id\":\"a02-b\",\"iso_path\":\"$A_ISO\",\"_pcv_caller_tenant\":\"ta\"}")"
expect_deny "3: bob vm.mount_iso a02-b a.iso + 위조 _pcv_caller_tenant=ta" "$r" not_allowed iso_path
audit_denied "#3 bob" "$M" vm.mount_iso bob
M="$(max_id)"
r="$(rpc "$TOK_NOB" vm.mount_iso "{\"vm_id\":\"a02-c\",\"iso_path\":\"$A_ISO\"}")"
expect_deny "3: nob vm.mount_iso a02-c a.iso(테넌트 없음)" "$r" no_tenant
audit_denied "#3 nob" "$M" vm.mount_iso nob

ln "$A_ISO" "$STATE/other/h.iso"
ln -s /etc/hostname "$STATE/disks/esc"
M="$(max_id)"
r="$(rpc "$TOK_ALICE" vm.mount_iso "{\"vm_id\":\"a02-a\",\"iso_path\":\"$STATE/other/h.iso\"}")"
expect_deny "4: alice vm.mount_iso 하드링크 \$STATE/other/h.iso" "$r" not_allowed iso_path
audit_denied "#4 하드링크" "$M" vm.mount_iso alice
M="$(max_id)"
r="$(rpc "$TOK_ALICE" vm.create "{\"name\":\"a02-esc\",\"storage_type\":\"qcow2\",\"image_dir\":\"$STATE/disks/esc/x\",$VMC}")"
expect_deny "4: alice vm.create image_dir=\$STATE/disks/esc/x(링크 너머)" "$r" not_allowed image_dir
audit_denied "#4 링크 너머" "$M" vm.create alice
r="$(rpc "$TOK_ALICE" vm.create "{\"name\":\"a02-new\",\"storage_type\":\"qcow2\",\"image_dir\":\"$STATE/disks/new/sub\",$VMC}")"
if [ "$(res "$r" '.accepted')" = "true" ]; then
    pass "4: alice vm.create image_dir=\$STATE/disks/new/sub(없는 하위) → accepted"
else
    fail "4: alice vm.create image_dir=\$STATE/disks/new/sub 기대 accepted — 응답 $(brief "$r")"
fi

M="$(max_id)"
r1="$(rpc "$TOK_BOB" vm.mount_iso "{\"vm_id\":\"a02-b\",\"iso_path\":\"$A_ISO\"}")"
audit_denied "#5 bob a.iso" "$M" vm.mount_iso bob
M="$(max_id)"
r2="$(rpc "$TOK_BOB" vm.mount_iso "{\"vm_id\":\"a02-b\",\"iso_path\":\"$NOPE_ISO\"}")"
audit_denied "#5 bob nope.iso" "$M" vm.mount_iso bob
m1="$(emsg "$r1")"; m2="$(emsg "$r2")"
if [ "$(ecode "$r1")" = "-32006" ] && [ "$(ecode "$r2")" = "-32006" ] && [ -n "$m1" ] && [ "$m1" = "$m2" ]; then
    pass "5: bob 남의 a.iso·없는 nope.iso 오류 error.message 바이트 같음(−32006 \"$m1\")"
else
    fail "5: bob a.iso $(brief "$r1") / nope.iso $(brief "$r2") — 문구가 같아야 한다"
fi
r="$(rpc "$TOK_VIC" iso.list '{}')"
vic_names="$(res "$r" '[.[]?.name] | sort')"
if is_ok "$r" && [ "$vic_names" = '["a.iso"]' ]; then
    pass "5: vic(VIEWER, ta) iso.list = [a.iso]"
else
    fail "5: vic iso.list 기대 [a.iso] — ${vic_names:-?} $(brief "$r")"
fi
r="$(rpc "$TOK_BOB" iso.list '{}')"
bob_names="$(res "$r" '[.[]?.name] | sort')"
if is_ok "$r" && [ "$bob_names" = '[]' ]; then
    pass "5: bob(tb) iso.list = []"
else
    fail "5: bob iso.list 기대 [] — ${bob_names:-?} $(brief "$r")"
fi
r="$(uds template.create "{\"name\":\"a02tpl\",\"vcpu\":1,\"memory_mb\":512,\"disk_gb\":10,\"os_variant\":\"ubuntu22.04\",\"iso_path\":\"$A_ISO\"}")"
is_ok "$r" || info "5: UDS template.create a02tpl 실패 — $(brief "$r")"
r="$(rpc "$TOK_VIC" template.get '{"name":"a02tpl"}')"
if [ "$(res "$r" '.name')" = '"a02tpl"' ] && [ "$(res "$r" 'has("iso_path")')" = "false" ]; then
    pass "5: vic template.get a02tpl(ADMIN이 iso_path=a.iso로 만든 템플릿) — 결과에 iso_path 없음"
else
    fail "5: vic template.get a02tpl 기대 결과 있음·iso_path 없음 — $(brief "$r")"
fi

OVL='"network_mode":"tenant-overlay","qos_min_mbps":10,"qos_max_mbps":100'
M="$(max_id)"
r="$(rpc "$TOK_ALICE" vm.create "{\"name\":\"a02-ov\",$OVL,\"tenant\":\"tb\"}")"
expect_deny "10: alice vm.create tenant-overlay tenant=tb(남의 테넌트)" "$r" not_allowed tenant
audit_denied "#10 alice tb" "$M" vm.create alice
r="$(rpc "$TOK_ALICE" vm.create "{\"name\":\"a02-ov\",$OVL,\"tenant\":\"ta\"}")"
expect_not_deny "10: alice vm.create tenant-overlay tenant=ta(자기 테넌트)" "$r"
M="$(max_id)"
r="$(rpc "$TOK_NOB" vm.create "{\"name\":\"a02-ov-n\",$OVL,\"tenant\":\"tc\"}")"
expect_deny "10: nob vm.create tenant-overlay tenant=tc(테넌트 없음)" "$r" no_tenant
audit_denied "#10 nob tc" "$M" vm.create nob
r="$(uds vm.create "{\"name\":\"a02-ov-adm\",$OVL,\"tenant\":\"tb\"}")"
expect_not_deny "10: UDS(ADMIN) vm.create tenant-overlay tenant=tb" "$r"

M="$(max_id)"
r="$(rpc "$TOK_ALICE" container.create '{"name":"a02ct","image":"ubuntu:22.04","network_bridge":"br-mgmt"}')"
expect_deny "11: alice container.create network_bridge=br-mgmt" "$r" not_allowed network_bridge
audit_denied "#11 alice br-mgmt" "$M" container.create alice
r="$(rpc "$TOK_ALICE" container.create '{"name":"a02ct-o","image":"ubuntu:22.04"}')"
expect_not_deny "11: alice container.create network_bridge 생략" "$r"
r="$(uds container.create '{"name":"a02ct-a","image":"ubuntu:22.04","network_bridge":"br-mgmt"}')"
expect_not_deny "11: UDS(ADMIN) container.create network_bridge=br-mgmt" "$r"

M="$(max_id)"
r="$(rpc "$TOK_ALICE" container.create '{"name":"a02ct-b","image":"ubuntu:22.04","bridge":"br-mgmt"}')"
expect_deny "11d: alice container.create bridge=br-mgmt(network_bridge 별칭)" "$r" not_allowed network_bridge
audit_denied "#11d alice bridge 별칭" "$M" container.create alice
t11d="$(aq "SELECT target FROM audit_log WHERE id > $M AND method='container.create' AND error_code=-32006;")"
if [ "$t11d" = "network_bridge=br-mgmt" ]; then
    pass "11d: 감사 대상 'network_bridge=br-mgmt'(별칭이 network_bridge로 정규화된 뒤 판정)"
else
    fail "11d: 감사 대상 '${t11d:-?}'(기대 network_bridge=br-mgmt)"
fi
r="$(rpc "$TOK_ALICE" container.create '{"name":"a02ct-c","image":"ubuntu:22.04","bridge":"br-a","network_bridge":"br-b"}')"
if [ "$(ecode "$r")" = "-32602" ] && [ "$(emsg "$r")" = "bridge and network_bridge must match" ]; then
    pass "11e: alice container.create bridge=br-a·network_bridge=br-b → −32602 'bridge and network_bridge must match'"
else
    fail "11e: 기대 −32602 'bridge and network_bridge must match' — 응답 $(brief "$r")"
fi

for c in a02lg-bob:bob a02lg-ali:alice; do
    mkdir -p "$STATE/var-lib/lxc/${c%%:*}"
    printf 'lxc.uts.name = %s\n' "${c%%:*}" > "$STATE/var-lib/lxc/${c%%:*}/config"
    printf '%s' "${c#*:}" > "$STATE/var-lib/lxc/${c%%:*}/purecvisor.owner"
done
M="$(max_id)"
r="$(rpc "$TOK_ALICE" container.logs '{"name":"a02lg-bob"}')"
if [ "$(ecode "$r")" = "-32006" ] && [ "$(emsg "$r")" = "Permission denied: operators can access only containers they created" ]; then
    pass "13: alice container.logs a02lg-bob(소유자 bob) → −32006 'operators can access only containers they created'"
else
    fail "13: alice container.logs a02lg-bob 기대 −32006 — 응답 $(brief "$r")"
fi
audit_denied "#13 alice 남의 logs" "$M" container.logs alice
r="$(rpc "$TOK_ALICE" container.logs '{"name":"a02lg-ali"}')"
expect_not_deny "13: alice container.logs 자기 컨테이너 a02lg-ali(소유자 alice)" "$r"

UNREG_JSON="$STATE/unregistered.json"
pcvctl_iso resource unregistered --tenant ta --json > "$UNREG_JSON" 2> "$STATE/pcvctl.err"
rc_u=$?
info "7: pcvctl resource unregistered --tenant ta --json (rc=$rc_u) → $(jq -c '[.items[]? | "\(.kind):\(.ref)"]' "$UNREG_JSON" 2>/dev/null || head -c 200 "$UNREG_JSON")"
has_item() {
    jq -e --arg k "$1" --arg r "$2" '[.items[]? | select(.kind==$k and .ref==$r and .tenants==["ta"])] | length == 1' \
        "$UNREG_JSON" >/dev/null 2>&1
}
if [ "$rc_u" = 0 ] && has_item iso "$LEGACY_ISO" && has_item bridge br-legacy && has_item usb 1d6b:0002; then
    pass "7: unregistered ⊇ iso:legacy.iso·bridge:br-legacy·usb:1d6b:0002 (tenants [\"ta\"])"
else
    fail "7: unregistered 목록에 기대 항목 없음(rc=$rc_u) — $(head -c 400 "$UNREG_JSON" 2>/dev/null) $(head -c 200 "$STATE/pcvctl.err" 2>/dev/null)"
fi
ISO_LEGACY_NAME="$(jq -r --arg r "$LEGACY_ISO" '.items[]? | select(.kind=="iso" and .ref==$r) | .suggested_name' "$UNREG_JSON" 2>/dev/null)"
USB_NAME="$(jq -r '.items[]? | select(.kind=="usb" and .ref=="1d6b:0002") | .suggested_name' "$UNREG_JSON" 2>/dev/null)"
info "7: 제안 이름 — legacy.iso='${ISO_LEGACY_NAME:-}' usb='${USB_NAME:-}' (계획 표기: iso-legacy·usb-1d6b-0002)"
for round in 1 2; do
    out="$(pcvctl_iso --format=json resource import "$UNREG_JSON" 2>&1)"
    rc_i=$?
    if [ "$rc_i" = 0 ] && is_ok "$out"; then
        pass "7: pcvctl resource import(unregistered 출력 그대로, ${round}회째) ok — $(brief "$out")"
    else
        fail "7: pcvctl resource import ${round}회째 rc=$rc_i — $(printf '%s' "$out" | head -c 300)"
    fi
done
r="$(uds resource.revoke "{\"name\":\"${ISO_LEGACY_NAME:-iso-legacy}\",\"tenant\":\"ta\"}")"
if [ "$(ecode "$r")" = "-32002" ] && [[ "$(emsg "$r")" == *"in use by VM(s): a02-a" ]]; then
    pass "7: resource.revoke ${ISO_LEGACY_NAME:-iso-legacy} ta → −32002 \"$(emsg "$r")\""
else
    fail "7: resource.revoke iso-legacy ta 기대 −32002 in use by VM(s): a02-a — $(brief "$r")"
fi
r="$(uds resource.revoke "{\"name\":\"${ISO_LEGACY_NAME:-iso-legacy}\",\"tenant\":\"tb\"}")"
if [ "$(ecode "$r")" = "-32005" ]; then
    pass "7: resource.revoke ${ISO_LEGACY_NAME:-iso-legacy} tb(허용 없음) → −32005"
else
    fail "7: resource.revoke iso-legacy tb 기대 −32005 — $(brief "$r")"
fi
r="$(uds resource.unregister "{\"name\":\"${USB_NAME:-usb-1d6b-0002}\"}")"
if [ "$(ecode "$r")" = "-32002" ] && [[ "$(emsg "$r")" == *"in use by VM(s):"*"a02-n"* ]]; then
    pass "7: resource.unregister ${USB_NAME:-usb-1d6b-0002}(a02-n 정의의 USB) → −32002 \"$(emsg "$r")\""
else
    fail "7: resource.unregister usb 기대 −32002 a02-n — $(brief "$r")"
fi

M="$(max_id)"
r="$(uds host.device.list '{}')"
if is_ok "$r" && [ "$(res "$r" '[(.devices|type), (.vms|type)] | join(",")')" = '"array,array"' ] \
   && [ "$(res "$r" '[.devices[] | select(.id=="1d6b:0002" and .present==false and .blocked_reason=="not_present")] | length')" = 1 ] \
   && [ "$(res "$r" '[.devices[] | select(.id=="1d6b:0002") | .assignments[] | select(.vm=="a02-n" and .vm_visible==true)] | length')" = 1 ]; then
    pass "14: UDS(ADMIN) host.device.list — devices·vms 배열, 1d6b:0002 호스트에 없는 정의 행·할당 a02-n 보임"
else
    fail "14: UDS(ADMIN) host.device.list 기대 모양 아님 — $(brief "$r")"
fi
for who in vic alice; do
    if [ "$who" = vic ]; then tok="$TOK_VIC"; else tok="$TOK_ALICE"; fi
    r="$(rpc "$tok" host.device.list '{}')"
    rows="$(res "$r" '[.devices[] | select(.id=="1d6b:0002")] | length')"
    hidden="$(res "$r" '[.devices[] | select(.id=="1d6b:0002") | .assignments[] | select(.vm==null and .vm_visible==false)] | length')"
    named="$(res "$r" '[.devices[] | (.assignments[]?, .companions[]?.assignments[]?) | select(.vm=="a02-n")] | length')"
    if is_ok "$r" && [ "$rows" = 1 ] && [ "$hidden" = 1 ] && [ "$named" = 0 ]; then
        pass "14: $who host.device.list — 허용된 1d6b:0002 행 보임, 할당 vm null·vm_visible false(a02-n 이름 0회)"
    else
        fail "14: $who host.device.list 행 ${rows:-?}·가린 할당 ${hidden:-?}·a02-n 노출 ${named:-?} — $(brief "$r")"
    fi
done
r="$(rpc "$TOK_ALICE" host.device.list '{}')"
if [ "$(res "$r" '[.vms[].name] | (map(select(.=="a02-a")) | length == 1) and (map(select(.=="a02-b" or .=="a02-c" or .=="a02-n")) | length == 0)')" = true ]; then
    pass "14: alice vms — 자기 VM a02-a 있음, 남의 VM 없음 $(res "$r" '[.vms[].name]')"
else
    fail "14: alice vms 기대 a02-a 포함·남의 VM 없음 — $(res "$r" '.vms')"
fi

r="$(rpc "$TOK_VIC" host.device.list '{}')"
if [ "$(res "$r" '[.vms[].name] | (map(select(.=="a02-a")) | length == 1) and (map(select(.=="a02-b" or .=="a02-c" or .=="a02-n")) | length == 0)')" = true ]; then
    pass "14: vic(VIEWER·ta) vms — 같은 테넌트 alice 의 a02-a 보임, 남의 VM 없음(판정 D-5)"
else fail "14: vic vms 기대 a02-a 포함·남의 VM 없음 — $(res "$r" '.vms')"; fi
for who in bob nob; do
    if [ "$who" = bob ]; then tok="$TOK_BOB"; else tok="$TOK_NOB"; fi
    r="$(rpc "$tok" host.device.list '{}')"
    if is_ok "$r" && [ "$(res "$r" '.devices | length')" = 0 ]; then
        pass "14: $who host.device.list devices = [] (허용 장치 없음)"
    else
        fail "14: $who devices 기대 [] — $(brief "$r")"
    fi
done
r="$(iso_curl -s --max-time 30 -H "Authorization: Bearer $TOK_ALICE" "$BASE_URL/host/devices" 2>/dev/null || true)"
if is_ok "$r" && [ "$(res "$r" '.devices | type')" = '"array"' ]; then
    pass "14: alice REST GET /host/devices → devices 배열"
else
    fail "14: alice REST GET /host/devices — $(brief "$r")"
fi
sleep 0.5
n_den="$(aq "SELECT COUNT(*) FROM audit_log WHERE id > $M AND method='host.device.list' AND result='denied';")"
if [ "${n_den:-x}" = 0 ]; then pass "14: host.device.list 감사 denied 0행"
else fail "14: host.device.list 감사 denied ${n_den:-?}행(기대 0)"; dump_rows "id > $M AND method='host.device.list'"; fi

r1="$(uds auth.user.tenant.set '{"username":"nob","tenant":"tc"}')"
r2="$(uds auth.user.tenant.set '{"username":"nob","tenant":"tc"}')"
if is_ok "$r1" && [ "$(ecode "$r2")" = "-32002" ]; then
    pass "8: auth.user.tenant.set nob tc — 첫 번째 ok, 두 번째 −32002"
else
    fail "8: auth.user.tenant.set nob tc — 첫 번째 $(brief "$r1") / 두 번째 $(brief "$r2")"
fi
M="$(max_id)"
r="$(rpc "$TOK_NOB" vm.mount_iso "{\"vm_id\":\"a02-c\",\"iso_path\":\"$A_ISO\"}")"
expect_deny "8: 테넌트 지정 뒤 nob vm.mount_iso a02-c a.iso(tc 허용 없음)" "$r" not_allowed iso_path
audit_denied "#8 nob tc" "$M" vm.mount_iso nob

MSG_PCI_REQ='nic_type=sriov requires pci_addr'
MSG_PCI_BAD='Invalid pci_addr: must be a PCI address in DDDD:BB:SS.F or BB:SS.F form'
job_wait() {
    local st=""
    for _ in $(seq 1 60); do
        st="$(printf '%s' "$(uds jobs.get "{\"job_id\":\"$1\"}")" | jq -r '.result.status // empty' 2>/dev/null)"
        case "$st" in completed|failed|cancelled|interrupted) break ;; esac
        sleep 0.25
    done
    echo "$st"
}
expect_invalid() {
    if [ "$(ecode "$2")" = "-32602" ] && [ "$(emsg "$2")" = "$3" ]; then
        pass "$1: −32602 \"$3\""
    else
        fail "$1: 기대 −32602 \"$3\" — 응답 $(brief "$2")"
    fi
}
admin_accepted_cleanup() {
    local job st rd jd sd
    job="$(printf '%s' "$3" | jq -r '.result.job_id // empty' 2>/dev/null)"
    if [ -z "$job" ]; then
        fail "$1: 접수(job_id) 기대 — 응답 $(brief "$3")"; return
    fi
    st="$(job_wait "$job")"
    if [ "${4:-}" = required ] && [ "$st" != completed ]; then
        fail "$1: 접수 작업 ${job} 상태 '${st:-?}' 기대 completed"
    elif [ "${4:-}" = required ]; then
        pass "$1: 접수 작업 ${job} completed"
    else
        info "$1: 접수 작업 ${job} 상태 '${st:-?}'"
    fi
    if [ "$st" = completed ]; then
        rd="$(uds vm.delete "{\"vm_id\":\"$2\"}")"
        jd="$(printf '%s' "$rd" | jq -r '.result.job_id // empty' 2>/dev/null)"
        sd="$([ -n "$jd" ] && job_wait "$jd")"
        if [ "$sd" = completed ]; then
            pass "$1: 정의($2) 생김 → vm.delete 작업 ${jd} completed(정리)"
        else
            fail "$1: 정의($2) 정리 vm.delete 작업 '${jd:-?}' 상태 '${sd:-?}' — $(brief "$rd")"
        fi
    fi
}
PCIB='"vcpu":1,"memory_mb":512,"disk_size_gb":1'
PCI0="\"nic_type\":\"sriov\",\"pci_addr\":\"\",$PCIB"
M="$(max_id)"
r="$(rpc "$TOK_ALICE" vm.create "{\"name\":\"a02-pci0\",$PCI0}")"
expect_deny "12: alice vm.create nic_type=sriov pci_addr=\"\"(빈 값)" "$r" not_allowed pci_addr
audit_denied "#12 alice 빈 pci_addr" "$M" vm.create alice
r="$(uds vm.create "{\"name\":\"a02-pci0-adm\",$PCI0}")"
expect_invalid "12: UDS(ADMIN) vm.create nic_type=sriov pci_addr=\"\"" "$r" "$MSG_PCI_BAD"
r="$(uds vm.create "{\"name\":\"a02-pci1\",\"nic_type\":\"sriov\",$PCIB}")"
expect_invalid "12b: UDS(ADMIN) nic_type=sriov pci_addr 키 없음" "$r" "$MSG_PCI_REQ"
r="$(uds vm.create "{\"name\":\"a02-pci2\",\"nic_type\":\"sriov\",\"pci_addr\":\"garbage\",$PCIB}")"
expect_invalid "12c: UDS(ADMIN) nic_type=sriov pci_addr=garbage" "$r" "$MSG_PCI_BAD"
r="$(uds vm.create "{\"name\":\"a02-pci3\",\"nic_type\":\"sriov\",\"pci_addr\":\"01:00.0\",$PCIB}")"
admin_accepted_cleanup "12d: UDS(ADMIN) pci_addr=01:00.0(짧은 꼴)" a02-pci3 "$r" required
r="$(uds vm.create "{\"name\":\"a02-pci4\",\"nic_type\":\"sriov\",\"pci_addr\":\"0000:3B:00.1\",$PCIB}")"
admin_accepted_cleanup "12e: UDS(ADMIN) pci_addr=0000:3B:00.1(대문자 — 정규화 없으면 워커 방어가 거절해 failed)" a02-pci4 "$r" required
CRIT0="$(grep -c CRITICAL "$DLOG" 2>/dev/null || true)"
r="$(uds vm.create "{\"name\":\"a02-pci5\",\"nic_type\":\"sriov\",\"pci_addr\":{},$PCIB}")"
CRIT1="$(grep -c CRITICAL "$DLOG" 2>/dev/null || true)"
expect_invalid "12f: UDS(ADMIN) pci_addr={}(객체)" "$r" "$MSG_PCI_REQ"
if [ "${CRIT1:-0}" -eq "${CRIT0:-0}" ]; then
    pass "12f: 그 요청이 만든 GLib CRITICAL 줄 0(전 ${CRIT0:-0} / 후 ${CRIT1:-0})"
else
    fail "12f: 그 요청이 GLib CRITICAL $(( ${CRIT1:-0} - ${CRIT0:-0} ))줄을 냈다"
fi
r="$(uds vm.create "{\"name\":\"a02-pci6\",\"nic_type\":\"bridge\",\"pci_addr\":\"garbage\",$PCIB,\"qos_min_mbps\":10,\"qos_max_mbps\":100}")"
if is_rpc "$r" && [ "$(ecode "$r")" != "-32602" ] && [ "$(ecode "$r")" != "-32006" ]; then
    pass "12g: UDS(ADMIN) nic_type=bridge pci_addr=garbage → −32602·−32006 아님(지금과 같음) — $(brief "$r")"
else
    fail "12g: 기대 −32602·−32006 아님 — 응답 $(brief "$r")"
fi
[ -n "$(printf '%s' "$r" | jq -r '.result.job_id // empty' 2>/dev/null)" ] && admin_accepted_cleanup "12g" a02-pci6 "$r"

MSG_SRIOV_NOBR='nic_type=sriov cannot be used without a network bridge (network_bridge "none" would create the VM without its SR-IOV NIC)'
MSG_SRIOV_OVL='nic_type=sriov cannot be combined with network_mode "tenant-overlay" (the VM would be created without its SR-IOV NIC)'
PCI_OK='"nic_type":"sriov","pci_addr":"0000:3b:10.1"'
for call in 'resource.register|{"kind":"pci","name":"pci-a","ref":"0000:3b:10.1"}' \
            'resource.grant|{"name":"pci-a","tenant":"ta"}'; do
    r="$(uds "${call%%|*}" "${call#*|}")"
    is_ok "$r" || info "12h: ${call%%|*} 실패 — $(brief "$r")"
done
r="$(uds vm.create "{\"name\":\"a02-pci7\",$PCI_OK,\"network_bridge\":\"none\",$PCIB}")"
expect_invalid "12h: UDS(ADMIN) sriov + network_bridge none" "$r" "$MSG_SRIOV_NOBR"
[ "$(ecode "$r")" = "-32602" ] || admin_accepted_cleanup "12h(base)" a02-pci7 "$r"
r="$(rpc "$TOK_ALICE" vm.create "{\"name\":\"a02-pci8\",$PCI_OK,\"network_bridge\":\"none\",$PCIB}")"
expect_invalid "12i: alice sriov + network_bridge none" "$r" "$MSG_SRIOV_NOBR"
SRIOV_OVL="$PCI_OK,\"network_mode\":\"tenant-overlay\",\"tenant\":\"tb\""
r="$(uds vm.create "{\"name\":\"a02-pci9\",$SRIOV_OVL,\"qos_min_mbps\":10,\"qos_max_mbps\":100,$PCIB}")"
expect_invalid "12j: UDS(ADMIN) sriov + tenant-overlay" "$r" "$MSG_SRIOV_OVL"
[ "$(ecode "$r")" = "-32602" ] || admin_accepted_cleanup "12j(base)" a02-pci9 "$r"
r="$(uds vm.create "{\"name\":\"a02-pci10\",$SRIOV_OVL,$PCIB}")"
expect_invalid "12k: UDS(ADMIN) sriov + tenant-overlay, qos 없음(SLA 문구 아님)" "$r" "$MSG_SRIOV_OVL"
M="$(max_id)"
r="$(rpc "$TOK_ALICE" vm.create "{\"name\":\"a02-pci11\",$SRIOV_OVL,\"qos_min_mbps\":10,\"qos_max_mbps\":100,$PCIB}")"
expect_invalid "12l: alice sriov + tenant-overlay(tb 불허 — −32006 아님)" "$r" "$MSG_SRIOV_OVL"

n_fail=0
for _ in $(seq 1 20); do
    n_fail="$(aq "SELECT COUNT(*) FROM audit_log WHERE id > $M AND method='vm.create' AND username='alice' AND result='fail' AND error_code=-32602;")"
    [ "${n_fail:-0}" -ge 1 ] && break
    sleep 0.25
done
sleep 0.3
n_fail="$(aq "SELECT COUNT(*) FROM audit_log WHERE id > $M AND method='vm.create' AND username='alice' AND result='fail' AND error_code=-32602;")"
n_den="$(aq "SELECT COUNT(*) FROM audit_log WHERE id > $M AND method='vm.create' AND username='alice' AND result='denied';")"
if [ "${n_fail:-?}" = 1 ] && [ "${n_den:-?}" = 0 ]; then
    pass "12l: 감사 fail(−32602) 1행·denied 0행"
else
    fail "12l: 감사 기대 fail 1·denied 0 — fail=${n_fail:-?} denied=${n_den:-?}"
fi
r="$(uds vm.create "{\"name\":\"a02-pci12\",$SRIOV_OVL,\"network_bridge\":\"none\",\"qos_min_mbps\":10,\"qos_max_mbps\":100,$PCIB}")"
expect_invalid "12m: UDS(ADMIN) sriov + tenant-overlay + network_bridge none(A1 먼저)" "$r" "$MSG_SRIOV_NOBR"
[ "$(ecode "$r")" = "-32602" ] || admin_accepted_cleanup "12m(base)" a02-pci12 "$r"
uds resource.unregister '{"name":"pci-a"}' >/dev/null

sleep 1
TOTAL="$(aq "SELECT COUNT(*) FROM audit_log WHERE id > $BASE;")"
D="$(aq "SELECT COUNT(*) FROM audit_log WHERE id > $BASE AND username='-';")"
if [ "${D:-x}" = "0" ] && [ "${TOTAL:-0}" -gt 0 ]; then
    pass "9: id > $BASE 의 ${TOTAL}행 중 username='-' 0행"
else
    fail "9: id > $BASE 의 ${TOTAL:-?}행 중 username='-' ${D:-?}행"
    dump_rows "id > $BASE AND username='-'"
fi

kill_daemon

echo ""
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
printf "  결과: ${GREEN}PASS %d${NC} / ${RED}FAIL %d${NC}\n" "$PASS" "$FAIL"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
[ "$FAIL" -eq 0 ]
