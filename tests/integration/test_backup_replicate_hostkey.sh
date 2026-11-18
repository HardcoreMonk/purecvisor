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

. "$SCRIPT_DIR/lib/pcv_isolated_daemon.sh"

skip() { echo -e "${YELLOW}[SKIP]${NC} replicate-hostkey E2E: $*"; exit 0; }
for c in bwrap sqlite3 curl python3 setsid sha256sum; do command -v "$c" >/dev/null 2>&1 || skip "$c 미설치"; done
for b in /usr/bin/ssh /usr/bin/ssh-keyscan /usr/sbin/zfs; do [ -e "$b" ] || skip "$b 없음(대역 bind 지점)"; done

pcv_iso_capable || skip "$PCV_ISO_SKIP_REASON"
if [ ! -x "$DAEMON_BIN" ]; then
    info "데몬 바이너리 없음 — make single 시도"
    make -C "$REPO" single >/dev/null 2>&1 || true
fi
[ -x "$DAEMON_BIN" ] || skip "데몬 바이너리 빌드 실패 ($DAEMON_BIN)"

pcv_iso_check_bin "$DAEMON_BIN" || skip "$PCV_ISO_SKIP_REASON"

pick_free_port() {
    local p
    for p in 28386 29386 31386 33386 37386 41386 43386; do
        if ! ss -ltn 2>/dev/null | grep -qE ":${p}([[:space:]]|$)"; then
            echo "$p"; return 0
        fi
    done
    return 1
}
PORT="$(pick_free_port)" || skip "빈 포트 확보 실패"
BASE="http://127.0.0.1:$PORT/api/v1"

ADMIN_PW='ReplHostkeyE2ePw-not-for-prod'
POOL=pcvtestpool; VM=replvm
KEY_A='AAAAC3NzaC1lZDI1NTE5AAAAIAE0Z2xt6Yvu4H6ptdw9C/JSPmnrkHjESJ9R2GiQ33yw'; FP_A='SHA256:rBbYAUGcs8skM8lqu8SNzYTuKrv89qMai86nLA2VFZE'
KEY_B='AAAAC3NzaC1lZDI1NTE5AAAAIFjVDdBLZ4Mbgu1oSPMibGOEhTwCdW7ogbQvuzvNZeZt'; FP_B='SHA256:5qiAQJdiPP8DLHnwmaeHnl1yBGdH3/uGmt6v05D395s'
KEY_E='AAAAE2VjZHNhLXNoYTItbmlzdHAyNTYAAAAIbmlzdHAyNTYAAABBBD9Tcis1M/TDFWmj5HwRpeCSCVTwDI2zr4bKukU2wn4OGXS2h7CP750+c3xb+0fDJ9FQRqOz9Pa780yAz7P5I4w='
STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-replhk.XXXXXX")"
pcv_iso_check_state_fs "$STATE" || { rm -rf "$STATE"; exit 1; }
mkdir -p "$STATE/var-lib" "$STATE/etc" "$STATE/mockbin"
VL="$STATE/var-lib"; SOCK="$VL/daemon.sock"; KH="$VL/replication_known_hosts"; AUDIT_DB="$VL/pcv_audit.db"

kill_daemon() {
    [ -f "$STATE/bwrap.pid" ] || return 0
    local bp; bp="$(cat "$STATE/bwrap.pid" 2>/dev/null || true)"
    [ -n "${bp:-}" ] || return 0
    kill -- "-$bp" 2>/dev/null || true
    kill "$bp" 2>/dev/null || true
    for _ in $(seq 1 12); do
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

cat > "$STATE/mockbin/ssh" <<'MOCKEOF'
#!/bin/sh

printf '%s\n' "$*" >> /var/lib/purecvisor/ssh_mock.log
mode=$(cat /var/lib/purecvisor/ssh_mode 2>/dev/null || echo ok)
if [ "$mode" = hostkey ]; then
  printf '@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\r\n@    WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!     @\r\n@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\r\nHost key verification failed.\r\n' >&2
  exit 255
fi
if [ "$mode" = refused ]; then
  printf 'ssh: connect to host node-b port 22: Connection refused\r\n' >&2
  exit 255
fi
cmd=""; seen=""
for a in "$@"; do
  if [ -n "$seen" ]; then cmd="$cmd $a"; continue; fi
  case "$a" in *@*) seen=1 ;; esac
done
case "$cmd" in
  " true") exit 0 ;;
  " sudo zfs recv"*) cat > /var/lib/purecvisor/recv_stream.bin; exit 0 ;;
  " zfs list -t snapshot -H "*) exit 1 ;;
  " zfs list -H -o name -t snapshot "*) printf '%s\n' "${cmd##* }"; exit 0 ;;
  *) exit 0 ;;
esac
MOCKEOF
cat > "$STATE/mockbin/ssh-keyscan" <<'MOCKEOF'
#!/bin/sh

printf '%s\n' "$*" >> /run/uuidd/request
host=""; for a in "$@"; do host="$a"; done
echo "# $host:22 SSH-2.0-OpenSSH_mock"
while read -r t b; do [ -n "$t" ] && printf '%s %s %s\n' "$host" "$t" "$b"; done < /run/nscd/dbpcv-keyscan-fixture
exit 0
MOCKEOF
cat > "$STATE/mockbin/zfs" <<'MOCKEOF'
#!/bin/sh

printf 'ZFS %s\n' "$*" >> /var/lib/purecvisor/zfs_mock.log
last=""; for a in "$@"; do last="$a"; done
case "$1" in
  list) printf '%s@pcv-auto-1\n%s@pcv-auto-2\n' "$last" "$last"; exit 0 ;;
  send) printf 'MOCKSTREAM\n'; exit 0 ;;
  *) exit 0 ;;
esac
MOCKEOF
chmod +x "$STATE/mockbin/ssh" "$STATE/mockbin/ssh-keyscan" "$STATE/mockbin/zfs"

write_conf() {
    cat > "$STATE/etc/daemon.conf" <<EOF
[daemon]
socket_path = /var/lib/purecvisor/daemon.sock
rest_port = $PORT
admin_user = admin
admin_password = $ADMIN_PW
jwt_secret = repl-hostkey-e2e-fixed-secret-not-for-prod-0001
libvirt_uri = test:///default
log_level = info
drain_timeout = 1
[storage]
zvol_pool = $POOL
[cluster]
ssh_user = pcvrepl
EOF
}

iso_curl() { pcv_iso_exec "$(cat "$STATE/bwrap.pid" 2>/dev/null)" curl "$@"; }

boot() {
    setsid "$PCV_ISO_BWRAP" \
        "${PCV_ISO_BWRAP_ARGS[@]}" \
        --bind "$STATE/var-lib" /var/lib/purecvisor \
        --bind "$STATE/etc" /etc/purecvisor \
        --ro-bind "$STATE/mockbin/ssh" /usr/bin/ssh \
        --ro-bind "$STATE/mockbin/ssh-keyscan" /usr/bin/ssh-keyscan \
        --ro-bind "$STATE/mockbin/zfs" /usr/sbin/zfs \
        --bind "$VL/keyscan_mock.log" /run/uuidd/request \
        --ro-bind "$VL/keyscan.fixture" /run/nscd/dbpcv-keyscan-fixture \
        --setenv PCV_LIBVIRT_URI test:///default \
        --setenv PURECVISOR_LIBVIRT_URI test:///default \
        --chdir / \
        "$DAEMON_BIN" > "$STATE/daemon.log" 2>&1 < /dev/null &
    echo "$!" > "$STATE/bwrap.pid"
    local p=""
    for _ in $(seq 1 60); do
        [ -S "$SOCK" ] && break
        sleep 0.5
    done
    [ -S "$SOCK" ] || return 1
    for _ in $(seq 1 20); do
        p="$(printf '%s\n' '{"jsonrpc":"2.0","method":"vm.list","params":{},"id":"probe"}' | pcv_iso_uds_nc "$SOCK" 5 2>/dev/null || true)"
        [ -n "$p" ] && break
        sleep 0.5
    done
    [ -n "$p" ] || return 1
    for _ in $(seq 1 60); do
        pcv_iso_rest_ready "$(cat "$STATE/bwrap.pid" 2>/dev/null)" "$BASE/health" >/dev/null && return 0
        sleep 0.5
    done
    return 1
}

login_v2() {
    iso_curl -s --max-time 6 -X POST "$BASE/auth/token" \
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

rpc() { printf '{"jsonrpc":"2.0","method":"%s","params":%s,"id":1}\n' "$1" "$2" | pcv_iso_uds_nc "$SOCK" 40 2>/dev/null || true; }
jget() {
    python3 -c '
import json, sys
try: o = json.loads(sys.argv[1])
except Exception: print(""); sys.exit()
for k in sys.argv[2].split("."):
    if isinstance(o, list):
        try: o = o[int(k)]
        except Exception: o = None
    elif isinstance(o, dict): o = o.get(k)
    else: o = None
print("" if o is None else json.dumps(o) if isinstance(o, (dict, list, bool)) else o)' "$1" "$2"
}
jlen() { jget "$1" "$2" | python3 -c 'import json,sys
try: print(len(json.load(sys.stdin)))
except Exception: print(-1)'; }
wait_job() { local r="" st; for _ in $(seq 1 50); do r="$(rpc jobs.get "{\"job_id\":\"$1\"}")"; st="$(jget "$r" result.status)"; case "$st" in completed|failed) break ;; esac; sleep 0.3; done; printf '%s' "$r"; }
reset_logs() { : > "$VL/ssh_mock.log"; : > "$VL/zfs_mock.log"; : > "$VL/keyscan_mock.log"; rm -f "$VL/recv_stream.bin"; }
expect() { if [ "$2" = "$3" ]; then pass "$1 ($2)"; else fail "$1: got '$2' want '$3'"; fi; }
create_user() { iso_curl -s -o /dev/null -w '%{http_code}' --max-time 6 -X POST "$BASE/auth/users" -H "Authorization: Bearer $1" -H 'Content-Type: application/json' -d "{\"username\":\"$2\",\"password\":\"$3\",\"role\":\"$4\"}" 2>/dev/null || echo 000; }
rest_rpc() { iso_curl -s -o /dev/null -w '%{http_code}' --max-time 45 -X POST "$BASE/rpc" -H "Authorization: Bearer $1" -H 'Content-Type: application/json' -d "{\"jsonrpc\":\"2.0\",\"method\":\"$2\",\"params\":$3,\"id\":1}" 2>/dev/null || echo 000; }
replicate() { rpc backup.replicate "{\"name\":\"$VM\",\"target_node\":\"$1\"}"; }

echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  복제 SSH 호스트 키 고정 격리 E2E${NC}"
echo -e "${CYAN}  binary=$DAEMON_BIN${NC}"
echo -e "${CYAN}  port=$PORT  state=$STATE${NC}"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"

write_conf
printf 'ssh-ed25519 %s\necdsa-sha2-nistp256 %s\nssh-rsa %s\n' "$KEY_A" "$KEY_E" "$KEY_A" > "$VL/keyscan.fixture"
echo ok > "$VL/ssh_mode"; reset_logs
boot || { fail "격리 데몬 기동 실패"; tail -30 "$STATE/daemon.log"; exit 1; }
pass "격리 데몬 기동 (UDS·REST 응답, ssh·ssh-keyscan·zfs 대역)"

r="$(replicate node-b)"
expect "a error.code" "$(jget "$r" error.code)" -32012
expect "a ssh 호출 0" "$(wc -l < "$VL/ssh_mock.log")" 0
expect "a Job 없음" "$(rpc jobs.list '{}' | grep -o '"backup.replicate"' | wc -l)" 0

r="$(rpc backup.replication.hostkey.scan '{"target_node":"Node-B"}')"
expect "b scan state" "$(jget "$r" result.state)" untrusted
expect "b scan 정규 대상" "$(jget "$r" result.target_node)" node-b
expect "b scan 키 2개(형식 불일치 줄 버림)" "$(jlen "$r" result.keys)" 2
expect "b scan 지문" "$(jget "$r" result.keys.0.fingerprint)" "$FP_A"
r="$(rpc backup.replication.hostkey.trust "{\"target_node\":\"Node-B\",\"fingerprint\":\"$FP_A\"}")"
expect "b trust state" "$(jget "$r" result.state)" trusted
expect "b known_hosts 줄" "$(cat "$KH")" "node-b ssh-ed25519 $KEY_A"
expect "b known_hosts 0600" "$(stat -c %a "$KH")" 600
expect "b list 1행" "$(jlen "$(rpc backup.replication.hostkey.list '{}')" result.hosts)" 1
expect "b 재scan trusted" "$(jget "$(rpc backup.replication.hostkey.scan '{"target_node":"node-b"}')" result.state)" trusted
expect "b keyscan 기록 채널 양성 대조(scan·trust 재스캔·재scan = 3회, e 의 0 단언이 공허하지 않게)" "$(wc -l < "$VL/keyscan_mock.log")" 3
reset_logs
r="$(replicate node-b)"; job="$(jget "$r" result.job_id)"
[ -n "$job" ] && pass "b accepted ($job)" || fail "b accepted 아님: $r"
expect "b Job 완료" "$(jget "$(wait_job "$job")" result.status)" completed
expect "b 첫 ssh = 탐침" "$(head -1 "$VL/ssh_mock.log" | awk '{print $NF}')" true
opts="$(python3 - "$VL/ssh_mock.log" <<'PY'
import sys
req = ["-F /dev/null", "-o BatchMode=yes", "-o StrictHostKeyChecking=yes",
       "-o UserKnownHostsFile=/var/lib/purecvisor/replication_known_hosts",
       "-o GlobalKnownHostsFile=/dev/null", "-o HostKeyAlias=node-b", "-o CheckHostIP=no",
       "-o UpdateHostKeys=no", "-o IdentitiesOnly=yes", "-o ConnectTimeout=10",
       "-i /etc/purecvisor/cluster_id_ed25519", "pcvrepl@node-b"]
lines = [l for l in open(sys.argv[1]) if l.strip()]
bad = [f"{i}:{r}" for i, l in enumerate(lines) for r in req if r not in l]
print("MISSING " + ",".join(bad) if bad or not lines else f"ok {len(lines)}")
PY
)"
case "$opts" in ok*) pass "b 모든 ssh argv 에 4.1 옵션 ($opts)" ;; *) fail "b ssh argv: $opts" ;; esac
grep -q MOCKSTREAM "$VL/recv_stream.bin" 2>/dev/null && pass "b 전송 도달" || fail "b 원격 recv 입력 없음"

h0="$(sha256sum "$KH" | cut -d' ' -f1)"
r="$(rpc backup.replication.hostkey.trust "{\"target_node\":\"node-c\",\"fingerprint\":\"$FP_B\"}")"
expect "c error.code" "$(jget "$r" error.code)" -32013
expect "c 파일 불변" "$(sha256sum "$KH" | cut -d' ' -f1)" "$h0"

echo hostkey > "$VL/ssh_mode"; reset_logs
r="$(replicate node-b)"; job="$(jget "$r" result.job_id)"
j="$(wait_job "$job")"
expect "d Job 실패" "$(jget "$j" result.status)" failed
expect "d error_code" "$(jget "$(jget "$j" result.result)" error_code)" -32013
expect "d ssh 호출 = 1" "$(wc -l < "$VL/ssh_mock.log")" 1
expect "d 그 호출 = 탐침" "$(awk '{print $NF}' "$VL/ssh_mock.log")" true
expect "d zfs send 0" "$(grep -c '^ZFS send' "$VL/zfs_mock.log")" 0
[ ! -e "$VL/recv_stream.bin" ] && pass "d 원격 recv 0" || fail "d 원격 recv 실행됨"

echo ok > "$VL/ssh_mode"; printf 'ssh-ed25519 %s\n' "$KEY_B" > "$VL/keyscan.fixture"; reset_logs
h0="$(sha256sum "$KH" | cut -d' ' -f1)"
r="$(rpc backup.replication.hostkey.trust "{\"target_node\":\"node-b\",\"fingerprint\":\"$FP_B\"}")"
expect "e error.code" "$(jget "$r" error.code)" -32002
expect "e 스캔 없이 거절" "$(wc -l < "$VL/keyscan_mock.log")" 0
expect "e 파일 불변" "$(sha256sum "$KH" | cut -d' ' -f1)" "$h0"
expect "e scan mismatch" "$(jget "$(rpc backup.replication.hostkey.scan '{"target_node":"node-b"}')" result.state)" mismatch
printf 'ssh-ed25519 %s\necdsa-sha2-nistp256 %s\n' "$KEY_A" "$KEY_E" > "$VL/keyscan.fixture"

expect "f removed" "$(jget "$(rpc backup.replication.hostkey.remove '{"target_node":"node-b"}')" result.removed)" true
expect "f 재거절" "$(jget "$(replicate node-b)" error.code)" -32012
expect "f 미등록 remove" "$(jget "$(rpc backup.replication.hostkey.remove '{"target_node":"node-b"}')" error.code)" -32005

p="$(login_v2 admin "$ADMIN_PW")"; AA="${p%|*}"
[ -n "$AA" ] && pass "g admin 로그인" || fail "g admin 로그인 실패"
for u in viewer operator; do
    c="$(create_user "$AA" "hk_$u" 'ReplHostkeyRbacE2ePw1' "$u")"
    case "$c" in 200|201) ;; *) fail "g $u 생성 $c" ;; esac
    q="$(login_v2 "hk_$u" 'ReplHostkeyRbacE2ePw1')"; T="${q%|*}"
    for m in scan trust list remove; do
        expect "g $u $m" "$(rest_rpc "$T" "backup.replication.hostkey.$m" "{\"target_node\":\"node-b\",\"fingerprint\":\"$FP_A\"}")" 403
    done
done

expect "h 재등록" "$(jget "$(rpc backup.replication.hostkey.trust "{\"target_node\":\"node-b\",\"fingerprint\":\"$FP_A\"}")" result.state)" trusted
echo refused > "$VL/ssh_mode"; reset_logs
r="$(replicate node-b)"; job="$(jget "$r" result.job_id)"; j="$(wait_job "$job")"
expect "h Job 실패" "$(jget "$j" result.status)" failed
expect "h error_code" "$(jget "$(jget "$j" result.result)" error_code)" -32000
expect "h zfs send 0" "$(grep -c '^ZFS send' "$VL/zfs_mock.log")" 0

aud() { sqlite3 "$AUDIT_DB" "$1" 2>/dev/null || echo 0; }
for _ in $(seq 1 20); do
    n1="$(aud "SELECT count(*) FROM audit_log WHERE method='backup.replicate' AND result='fail' AND error_code=-32012;")"
    [ "${n1:-0}" -ge 2 ] && break; sleep 0.3
done
n2="$(aud "SELECT count(*) FROM audit_log WHERE method='backup.replicate' AND result='fail' AND error_code=-32013;")"
n3="$(aud "SELECT count(*) FROM audit_log WHERE method='backup.replication.hostkey.trust' AND result='fail';")"
n4="$(aud "SELECT count(*) FROM audit_log WHERE method='backup.replication.hostkey.scan' AND result='ok';")"
[ "${n1:-0}" -ge 2 ] && [ "${n2:-0}" -ge 1 ] && [ "${n3:-0}" -ge 2 ] && [ "${n4:-0}" -ge 3 ] \
    && pass "감사 행 (−32012 $n1, −32013 $n2, trust fail $n3, scan ok $n4)" \
    || fail "감사 행 부족 (−32012 $n1, −32013 $n2, trust fail $n3, scan ok $n4)"

n5="$(aud "SELECT count(*) FROM audit_log WHERE method='backup.replication.hostkey.remove' AND result='ok' AND target='node-b';")"
n6="$(aud "SELECT count(*) FROM audit_log WHERE method='backup.replication.hostkey.remove' AND result='fail' AND error_code=-32005 AND target='node-b';")"
n7="$(aud "SELECT count(*) FROM audit_log WHERE method='backup.replication.hostkey.remove' AND result IN ('ok','fail') AND COALESCE(target,'')<>'node-b';")"
[ "${n5:-0}" -ge 1 ] && [ "${n6:-0}" -ge 1 ] && [ "${n7:-1}" -eq 0 ] \
    && pass "감사 remove 행 target=node-b (ok $n5, fail −32005 $n6, 다른 target $n7)" \
    || fail "감사 remove 행 target 불일치 (ok $n5, fail −32005 $n6, 다른 target $n7)"

info "trust 감사 행위자: $(aud "SELECT COALESCE(username,'NULL') FROM audit_log WHERE method='backup.replication.hostkey.trust' ORDER BY id DESC LIMIT 1;")"

kill_daemon; rm -f "$STATE/bwrap.pid"
echo ""
printf "  결과: ${GREEN}PASS %d${NC} / ${RED}FAIL %d${NC}\n" "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
