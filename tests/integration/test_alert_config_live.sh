#!/usr/bin/env bash
                                                                       
                                                    
                                                                   
                                                        
 
                                              
                                                        
                                                 
 
        
                                                     
                                             
                                           
                                                             
                                                             
 
     
                                                                                 

set -Eeuo pipefail

readonly SERVICE="purecvisorsd.service"
readonly CONFIG="/etc/purecvisor/daemon.conf"
readonly SOCKET="/var/run/purecvisor/daemon.sock"
readonly CLI="/usr/local/bin/pcvctl"
readonly EXCLUDED_ADDRESS="192.0.2.100"

BACKUP=""
ORIGINAL_SHA=""
RESTORE_NEEDED=0
PASS_COUNT=0

log()  { printf '[R7/ADR-0032] %s\n' "$*"; }
pass() { PASS_COUNT=$((PASS_COUNT + 1)); log "PASS $PASS_COUNT — $*"; }
die()  { log "FAIL — $*" >&2; exit 1; }

wait_ready()
{
    local attempt
    for attempt in $(seq 1 100); do
        if systemctl is-active --quiet "$SERVICE" && [[ -S "$SOCKET" ]]; then
            if rpc '{"jsonrpc":"2.0","method":"alert.config.get","params":{},"id":"ready"}' \
                >/dev/null 2>&1; then
                return 0
            fi
        fi
        sleep 0.2
    done
    return 1
}

                                                            
                                                   
rpc()
{
    local payload="$1"
    python3 - "$SOCKET" 3< <(printf '%s' "$payload") <<'PY'
import socket
import sys

path = sys.argv[1]
payload = open(3, "rb", closefd=False).read()
client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
client.settimeout(5)
client.connect(path)
client.sendall(payload)
client.shutdown(socket.SHUT_WR)
chunks = []
while True:
    chunk = client.recv(65536)
    if not chunk:
        break
    chunks.append(chunk)
client.close()
sys.stdout.write(b"".join(chunks).decode("utf-8"))
PY
}

json_path()
{
    local path="$1"
    python3 -c '
import json, sys
value = json.load(sys.stdin)
for key in sys.argv[1].split("."):
    value = value[key]
if isinstance(value, bool):
    print("true" if value else "false")
elif value is None:
    print("")
else:
    print(value)
' "$path"
}

field()
{
    local json="$1"
    local path="$2"
    printf '%s' "$json" | json_path "$path"
}

assert_field()
{
    local json="$1"
    local path="$2"
    local expected="$3"
    local label="$4"
    local actual
    actual="$(field "$json" "$path")" || die "$label: '$path' 필드가 없다"
    [[ "$actual" == "$expected" ]] \
        || die "$label: expected '$expected', got '$actual'"
}

                                                       
                                           
write_test_alert_source()
{
    local warn_value="$1"
    local crit_value="$2"
    if grep -qE '^[[:space:]]*\[alert\][[:space:]]*$' "$BACKUP"; then
        die "기존 [alert] 섹션이 있어 자동 변형을 중단한다"
    fi
    cp -a -- "$BACKUP" "$CONFIG"
    printf '\n[alert]\nenabled = false\ncpu_warn = %s\ncpu_crit = %s\n' \
        "$warn_value" "$crit_value" >>"$CONFIG"
}

restore_original()
{
    [[ "$RESTORE_NEEDED" -eq 1 ]] || return 0
    set +e
    log "원본 daemon.conf 복구 및 서비스 정상화"
    if [[ -n "$BACKUP" && -f "$BACKUP" ]]; then
        cp -a -- "$BACKUP" "$CONFIG"
        systemctl restart "$SERVICE"
        wait_ready
    fi
    RESTORE_NEEDED=0
}

trap restore_original EXIT INT TERM

[[ "$EUID" -eq 0 ]] || die "root 권한이 필요하다"
[[ "${PCV_R7_ALERT_LIVE:-}" == "1" ]] \
    || die "PCV_R7_ALERT_LIVE=1 명시 승인이 필요하다"
[[ -f "$CONFIG" ]] || die "$CONFIG 파일이 없다"
[[ -x "$CLI" ]] || die "$CLI 실행 파일이 없다"
command -v python3 >/dev/null || die "python3가 없다"
command -v sha256sum >/dev/null || die "sha256sum이 없다"

if ip -o -4 addr show 2>/dev/null | awk '{print $4}' \
    | grep -qE "^${EXCLUDED_ADDRESS//./\\.}/"; then
    die "제외 노드 $EXCLUDED_ADDRESS 에서는 실행할 수 없다"
fi
systemctl is-active --quiet "$SERVICE" || die "$SERVICE 가 active가 아니다"
wait_ready || die "라이브 UDS가 응답하지 않는다"

install -d -o root -g root -m 0700 /var/backups/purecvisor
BACKUP="/var/backups/purecvisor/daemon.conf.r7-alert.$(date +%Y%m%dT%H%M%S).bak"
cp -a -- "$CONFIG" "$BACKUP"
chmod 0600 "$BACKUP"
ORIGINAL_SHA="$(sha256sum "$BACKUP" | awk '{print $1}')"
RESTORE_NEEDED=1
log "복구용 원본을 root 전용 백업으로 보존했다: $BACKUP"

BASE="$(rpc '{"jsonrpc":"2.0","method":"alert.config.get","params":{},"id":"base"}')"
BASE_REV="$(field "$BASE" result.config_revision)"
BASE_WARN="$(field "$BASE" result.cpu_warn)"
BASE_CRIT="$(field "$BASE" result.cpu_crit)"
assert_field "$BASE" result.daemon_config_valid true "초기 daemon source"

if (( BASE_WARN + 1 < BASE_CRIT )); then
    TARGET_WARN=$((BASE_WARN + 1))
elif (( BASE_WARN > 0 )); then
    TARGET_WARN=$((BASE_WARN - 1))
else
    die "유효한 대체 cpu_warn 값을 만들 수 없다"
fi

                                                            
SET_A="$(rpc "{\"jsonrpc\":\"2.0\",\"method\":\"alert.config.set\",\"params\":{\"expected_revision\":$BASE_REV,\"cpu_warn\":$TARGET_WARN},\"id\":\"cas-a\"}")"
REV_A=$((BASE_REV + 1))
assert_field "$SET_A" result.config_revision "$REV_A" "Client A revision"
assert_field "$SET_A" result.cpu_warn "$TARGET_WARN" "Client A 값"
pass "Client A CAS 성공 및 revision 1회 증가"

STALE="$(rpc "{\"jsonrpc\":\"2.0\",\"method\":\"alert.config.set\",\"params\":{\"expected_revision\":$BASE_REV,\"cpu_warn\":$BASE_WARN},\"id\":\"cas-b\"}")"
assert_field "$STALE" error.code -32002 "Client B stale conflict"
pass "Client B stale revision 충돌"

INVALID="$(rpc "{\"jsonrpc\":\"2.0\",\"method\":\"alert.config.set\",\"params\":{\"expected_revision\":$REV_A,\"cpu_warn\":50,\"cpu_crit\":50},\"id\":\"invalid\"}")"
assert_field "$INVALID" error.code -32602 "동일 임계값 거절"
AFTER_REJECT="$(rpc '{"jsonrpc":"2.0","method":"alert.config.get","params":{},"id":"after-reject"}')"
assert_field "$AFTER_REJECT" result.config_revision "$REV_A" "거절 후 revision"
assert_field "$AFTER_REJECT" result.cpu_warn "$TARGET_WARN" "거절 후 값"
pass "잘못된 설정의 원자적 거절 및 상태 불변"

RESTORE_RUNTIME="$(rpc "{\"jsonrpc\":\"2.0\",\"method\":\"alert.config.set\",\"params\":{\"expected_revision\":$REV_A,\"cpu_warn\":$BASE_WARN},\"id\":\"runtime-restore\"}")"
REV_RUNTIME_RESTORED=$((REV_A + 1))
assert_field "$RESTORE_RUNTIME" result.config_revision "$REV_RUNTIME_RESTORED" "런타임 원복 revision"

                                                  
CLI_RESULT="$($CLI --socket="$SOCKET" --format=json --no-color \
    alert set --cpu_warn "$TARGET_WARN" 2>/dev/null)"
REV_CLI=$((REV_RUNTIME_RESTORED + 1))
assert_field "$CLI_RESULT" result.config_revision "$REV_CLI" "CLI revision"
assert_field "$CLI_RESULT" result.cpu_warn "$TARGET_WARN" "CLI 값"
CLI_RESTORE="$(rpc "{\"jsonrpc\":\"2.0\",\"method\":\"alert.config.set\",\"params\":{\"expected_revision\":$REV_CLI,\"cpu_warn\":$BASE_WARN},\"id\":\"cli-restore\"}")"
REV_BEFORE_RELOAD=$((REV_CLI + 1))
assert_field "$CLI_RESTORE" result.config_revision "$REV_BEFORE_RELOAD" "CLI 원복 revision"
pass "설치 CLI GET→SET 및 정확한 1회 증가"

                                                               
write_test_alert_source "$TARGET_WARN" "$BASE_CRIT"
VALID_RELOAD="$(rpc '{"jsonrpc":"2.0","method":"alert.config.reload","params":{},"id":"reload-valid"}')"
REV_VALID_RELOAD=$((REV_BEFORE_RELOAD + 1))
assert_field "$VALID_RELOAD" result.config_revision "$REV_VALID_RELOAD" "유효 reload revision"
assert_field "$VALID_RELOAD" result.cpu_warn "$TARGET_WARN" "유효 reload 값"
assert_field "$VALID_RELOAD" result.daemon_config_valid true "유효 reload source"
pass "유효 daemon source reload commit"

write_test_alert_source abc "$BASE_CRIT"
INVALID_RELOAD="$(rpc '{"jsonrpc":"2.0","method":"alert.config.reload","params":{},"id":"reload-invalid"}')"
assert_field "$INVALID_RELOAD" error.code -32602 "무효 reload 응답"
AFTER_INVALID_RELOAD="$(rpc '{"jsonrpc":"2.0","method":"alert.config.get","params":{},"id":"reload-state"}')"
assert_field "$AFTER_INVALID_RELOAD" result.config_revision "$REV_VALID_RELOAD" "무효 reload revision 불변"
assert_field "$AFTER_INVALID_RELOAD" result.cpu_warn "$TARGET_WARN" "무효 reload 런타임 불변"
assert_field "$AFTER_INVALID_RELOAD" result.daemon_config_valid false "무효 reload source 상태"
assert_field "$AFTER_INVALID_RELOAD" result.daemon_config_error invalid_alert_config "무효 reload 오류"
pass "무효 reload 거절, 런타임 보존, source 경고 노출"

                                   
cp -a -- "$BACKUP" "$CONFIG"
SOURCE_RESTORE="$(rpc '{"jsonrpc":"2.0","method":"alert.config.reload","params":{},"id":"source-restore"}')"
REV_SOURCE_RESTORED=$((REV_VALID_RELOAD + 1))
assert_field "$SOURCE_RESTORE" result.config_revision "$REV_SOURCE_RESTORED" "source 원복 revision"
assert_field "$SOURCE_RESTORE" result.cpu_warn "$BASE_WARN" "source 원복 값"
assert_field "$SOURCE_RESTORE" result.daemon_config_valid true "source 원복 상태"
pass "원본 source reload 복구"

                                                     
write_test_alert_source abc "$BASE_CRIT"
systemctl restart "$SERVICE"
wait_ready || die "무효 source 재시작 뒤 서비스가 응답하지 않는다"
BAD_START="$(rpc '{"jsonrpc":"2.0","method":"alert.config.get","params":{},"id":"bad-start"}')"
assert_field "$BAD_START" result.config_revision 1 "무효 startup revision"
assert_field "$BAD_START" result.enabled false "무효 startup 안전 비활성"
assert_field "$BAD_START" result.cpu_warn 80 "무효 startup 안전 기본값"
assert_field "$BAD_START" result.daemon_config_valid false "무효 startup source 상태"
assert_field "$BAD_START" result.daemon_config_error invalid_alert_config "무효 startup 오류"
pass "무효 startup의 안전 기본값과 revision reset"

                                                       
cp -a -- "$BACKUP" "$CONFIG"
systemctl restart "$SERVICE"
wait_ready || die "최종 원복 재시작 뒤 서비스가 응답하지 않는다"
FINAL="$(rpc '{"jsonrpc":"2.0","method":"alert.config.get","params":{},"id":"final"}')"
assert_field "$FINAL" result.config_revision 1 "최종 startup revision"
assert_field "$FINAL" result.cpu_warn "$BASE_WARN" "최종 startup 값"
assert_field "$FINAL" result.daemon_config_valid true "최종 startup source"
[[ "$(sha256sum "$CONFIG" | awk '{print $1}')" == "$ORIGINAL_SHA" ]] \
    || die "최종 daemon.conf 해시가 원본과 다르다"
systemctl is-active --quiet "$SERVICE" || die "최종 서비스가 active가 아니다"
RESTORE_NEEDED=0
trap - EXIT INT TERM
pass "원본 파일 해시, 서비스 active, 정상 startup 복구"

log "완료: $PASS_COUNT개 라이브 계약 통과; 복구 백업=$BACKUP"
