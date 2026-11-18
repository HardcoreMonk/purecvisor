#!/usr/bin/env bash
                                                                                                                      
                                                                                                          
                                                                                  
                                                                                                
                                                                                                   
                                                  
                                                                               
                                                   
                                                                                    
 
                                                                                                             
        
                                                                                               
                                    
                                                                                                
                                                                     
                                                                                                            
                                                                                                                  
                                                                 
                                                      
                                                                                                                        
                                                                    
                                                                                                        
                                            
                                                                                                              
                                                                                       
                                                                                                                  
                                                                                       
                                                                                                

set -uo pipefail

GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS=0; FAIL=0
pass() { echo -e "${GREEN}[PASS]${NC} $*"; PASS=$((PASS+1)); }
fail() { echo -e "${RED}[FAIL]${NC} $*"; FAIL=$((FAIL+1)); }
info() { echo -e "${CYAN}[INFO]${NC} $*"; }
check() { if [ "$1" = "$2" ]; then pass "$3"; else fail "$3 (기대 '$2', 실제 '$1')"; fi; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
DAEMON_BIN="${PCV_DAEMON_BIN:-$REPO/bin/purecvisorsd}"
MODE="${MODE:-normal}"
                                                                
. "$SCRIPT_DIR/lib/pcv_isolated_daemon.sh"

skip() { echo -e "${YELLOW}[SKIP]${NC} VM 읽기 범위 격리 E2E: $*"; exit 0; }

for c in bwrap sqlite3 curl python3 jq setsid node; do command -v "$c" >/dev/null 2>&1 || skip "$c 미설치"; done
pcv_iso_capable || skip "$PCV_ISO_SKIP_REASON"
[ -x "$DAEMON_BIN" ] || skip "데몬 바이너리 없음 ($DAEMON_BIN — make single)"
pcv_iso_check_bin "$DAEMON_BIN" || skip "$PCV_ISO_SKIP_REASON"

pick_free_port() {
    local p
    for p in "$@"; do
        ss -ltn 2>/dev/null | grep -qE ":${p}([[:space:]]|$)" || { echo "$p"; return 0; }
    done
    return 1
}
PORT="$(pick_free_port 28413 29413 31413 33413 37413 41413)" || skip "빈 포트 확보 실패"
BASE_URL="http://127.0.0.1:$PORT/api/v1"
XFF=(-H 'X-Forwarded-For: 192.0.2.10')                                                    

                                                      
STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-vmreadscope.XXXXXX")"
pcv_iso_check_state_fs "$STATE" || { rm -rf "$STATE"; exit 1; }
mkdir -p "$STATE/var-lib" "$STATE/etc" "$STATE/pw" "$STATE/ws"
chmod 700 "$STATE/pw"
SOCK="$STATE/var-lib/daemon.sock"
AUDIT_DB="$STATE/var-lib/pcv_audit.db"
DLOG="$STATE/daemon.log"
LIBVIRT_URI='test:///default'
VM=test
NOPE=pcv-nope-vm
pw_new() {                                          
    local f="$STATE/pw/$1"
    ( umask 077; python3 -c 'import secrets; print("E2e-" + secrets.token_urlsafe(18) + "-9a", end="")' > "$f" )
    printf '%s' "$f"
}
ADMIN_PWF="$(pw_new admin)"
cat > "$STATE/etc/daemon.conf" <<CONF
[daemon]
socket_path = /var/lib/purecvisor/daemon.sock
rest_port = $PORT
admin_user = admin
admin_password = $(cat "$ADMIN_PWF")
jwt_secret = vmreadscope-e2e-fixed-secret-not-for-prod-01
libvirt_uri = $LIBVIRT_URI
log_level = info
drain_timeout = 1
pool_max_conn = 2
CONF
chmod 600 "$STATE/etc/daemon.conf"

kill_daemon() {                                                    
    local f="$STATE/bwrap.pid" bp
    [ -f "$f" ] || return 0
    bp="$(cat "$f" 2>/dev/null || true)"
    [ -n "${bp:-}" ] || return 0
    kill -- "-$bp" 2>/dev/null || true
    kill "$bp" 2>/dev/null || true
    for _ in $(seq 1 12); do kill -0 "$bp" 2>/dev/null || break; sleep 0.25; done
    kill -9 -- "-$bp" 2>/dev/null || true
    kill -9 "$bp" 2>/dev/null || true
    rm -f "$f"
}
WS_PIDS=()
cleanup() { for p in "${WS_PIDS[@]:-}"; do [ -n "$p" ] && kill "$p" 2>/dev/null; done; kill_daemon; rm -rf "$STATE" 2>/dev/null || true; }
trap cleanup EXIT INT TERM

BP() { cat "$STATE/bwrap.pid" 2>/dev/null; }
iso_curl() { pcv_iso_exec "$(BP)" curl "$@"; }
uds() {                                                                        
    printf '%s' "{\"jsonrpc\":\"2.0\",\"method\":\"$1\",\"params\":$2,\"id\":\"vr\"}" \
        | pcv_iso_uds_nc "$SOCK" "${3:-30}" 2>/dev/null || true
}
                                                      
role_of() { case "$1" in va|vn) echo 0 ;; adma) echo 2 ;; *) echo 1 ;; esac; }
tenant_of() { case "$1" in va|oa1|oa2|adma) echo '"ta"' ;; ob) echo '"tb"' ;; *) echo null ;; esac; }
as() {                                                      
    local p
    p="$(printf '%s' "$3" | jq -c --argjson r "$(role_of "$1")" --arg s "$1" --argjson t "$(tenant_of "$1")" \
        '. + {_pcv_caller_role:$r, _pcv_caller_sub:$s, _pcv_caller_tenant:$t}')"
    uds "$2" "$p"
}
ecode() { printf '%s' "$1" | jq -r '.error.code // empty' 2>/dev/null; }
emsg() { printf '%s' "$1" | jq -r '.error.message // empty' 2>/dev/null; }
res() { printf '%s' "$1" | jq -c "(if has(\"result\") then .result else .data end) | $2" 2>/dev/null; }
mentions_vm() {                                                
    printf '%s' "$1" | jq -e --arg v "$VM" --arg u "vm:${UUID:-none}" '[.. | strings | select(. == $v or . == $u)] | length > 0' \
        >/dev/null 2>&1 && echo yes || echo no
}
login_token() {                                                     
    local body
    body="$(jq -nc --arg u "$1" --rawfile p "$2" '{username:$u, password:$p}')"
    iso_curl -s --max-time 6 -X POST "$BASE_URL/auth/token" -H 'Content-Type: application/json' -d "$body" 2>/dev/null \
        | jq -r '.access_token // empty' 2>/dev/null || true
}
http_code() { iso_curl -s --max-time 10 -o "$STATE/body.out" -w '%{http_code}' "$@" 2>/dev/null || true; }
set_owner() { uds vm.owner.set "{\"name\":\"$VM\",\"owner\":\"$1\"}"; }

boot() {
    setsid "$PCV_ISO_BWRAP" "${PCV_ISO_BWRAP_ARGS[@]}" \
        --bind "$STATE/var-lib" /var/lib/purecvisor --bind "$STATE/etc" /etc/purecvisor --bind "$STATE" "$STATE" \
        --setenv PCV_LIBVIRT_URI "$LIBVIRT_URI" --setenv PURECVISOR_LIBVIRT_URI "$LIBVIRT_URI" --chdir / \
        "$DAEMON_BIN" > "$DLOG" 2>&1 < /dev/null &
    echo "$!" > "$STATE/bwrap.pid"
    for _ in $(seq 1 60); do
        if [ -S "$SOCK" ]; then
            case "$(uds vm.list '{}' 10)" in
                *'"jsonrpc"'*) pcv_iso_rest_ready "$(BP)" "$BASE_URL/health" >/dev/null && return 0 ;;
            esac
        fi
        sleep 0.5
    done
    return 1
}

echo -e "${CYAN}  VM 읽기 범위 격리 E2E  binary=$DAEMON_BIN  mode=$MODE  port=$PORT${NC}"
boot || { fail "S1: 격리 데몬 기동 실패"; tail -30 "$DLOG"; exit 1; }
pass "S1: 격리 데몬 기동 + UDS + REST /health"

                                                                 
declare -A PWF
for spec in va:viewer:ta oa1:operator:ta oa2:operator:ta ob:operator:tb vn:viewer: on:operator:; do
    IFS=: read -r u r t <<< "$spec"
    PWF[$u]="$(pw_new "$u")"
    body="$(jq -nc --arg u "$u" --rawfile p "${PWF[$u]}" --arg r "$r" --arg t "$t" \
        '{username:$u, password:$p, role:$r} + (if $t == "" then {} else {tenant:$t} end)')"
    R="$(uds auth.user.create "$body")"
    [ -z "$(ecode "$R")" ] || { fail "S2: 계정 $u 생성 실패($(ecode "$R"))"; exit 1; }
done
pass "S2: 계정 여섯 생성(va·oa1·oa2·ob·vn·on)"

R="$(set_owner oa1)"
if [ "$(ecode "$R")" = "-32601" ]; then
    info "vm.owner.set −32601(이 데몬에 없음) — MODE=$MODE: 소유자 없이 계속(반사실 기록용)"
    [ "$MODE" = "counterfactual" ] || { fail "S3: vm.owner.set 라우트 없음"; exit 1; }
else
    check "$(res "$R" .owner | tr -d '"')" "oa1" "S3: vm.owner.set $VM → oa1"
fi
UUID="$(uds vm.list '{}' | jq -r --arg v "$VM" '[(.result // .data)[]? | select(.name == $v) | .uuid][0] // empty')"
[ -n "$UUID" ] || { fail "S4: $VM UUID 를 못 읽음"; exit 1; }

                                                            
READS=(vm.snapshot.list:name vm.blkio.get:name vm.usb.list:name vm.sla.report:name vm.cpu.stats:name vm.memory.stats:name
       device.nic.list:vm_id qos.vm.get:vm backup.history:vm_name vm.metrics:name vm.guest.agent.status:name
       vm.guest.fsinfo:name vm.guest.ping:name monitor.metrics:vm_id)
DENY='Permission denied: VM not found or not visible'
ROLE='Permission denied: insufficient role for this method'                                                
bad1=(); gated=(); VIEW_M=""; VIEW_K=""
for mk in "${READS[@]}"; do
    m="${mk%%:*}"; k="${mk#*:}"
    for a in oa2 va; do
        e="$(emsg "$(as "$a" "$m" "{\"$k\":\"$VM\"}")")"
        if [ "$e" = "$ROLE" ]; then gated+=("$m:$a"); continue; fi
        [ "$e" = "$DENY" ] && bad1+=("$m:$a 거절됨")
        [ "$a" = va ] && [ -z "$VIEW_M" ] && { VIEW_M="$m"; VIEW_K="$k"; }
    done
    for a in ob vn; do
        Rv="$(as "$a" "$m" "{\"$k\":\"$VM\"}")"; Rn="$(as "$a" "$m" "{\"$k\":\"$NOPE\"}")"
        case "$(emsg "$Rv")" in "$DENY") ;; "$ROLE") gated+=("$m:$a") ;; *) bad1+=("$m:$a 통과됨") ;; esac
        [ "$Rv" = "$Rn" ] || bad1+=("$m:$a 응답이 없는 VM 과 다름")
    done
done
[ ${#gated[@]} -gt 0 ] && info "1a: 정책표 역할 거절(범위 판정 전) ${#gated[@]}건 — ${gated[*]}"
[ ${#bad1[@]} -eq 0 ] && pass "1a: 단일 읽기 14 — 허용 계정 통과, ob·vn 거절이 없는 VM 과 바이트 동일" \
    || fail "1a: 단일 읽기 — ${bad1[*]}"
[ -n "$VIEW_M" ] || { fail "1a: VIEWER 가 부를 수 있는 단일 읽기가 없다"; VIEW_M=vm.snapshot.list; VIEW_K=name; }
sleep 1
                                                              
odd="$(sqlite3 -cmd '.timeout 5000' "$AUDIT_DB" "SELECT method||':'||username FROM audit_log WHERE username IN ('ob','vn')
    AND target IN ('$VM','$NOPE') GROUP BY method, username HAVING COUNT(DISTINCT result||':'||IFNULL(error_code,'')) <> 1
    OR COUNT(DISTINCT target) <> 2;" 2>/dev/null)"
n_aud="$(sqlite3 -cmd '.timeout 5000' "$AUDIT_DB" "SELECT COUNT(*) FROM audit_log WHERE username IN ('ob','vn')
    AND target IN ('$VM','$NOPE') AND result='denied' AND error_code=-32006;" 2>/dev/null)"
if [ -z "$odd" ] && [ "${n_aud:-0}" -ge 56 ]; then pass "1b: 거절 감사 행 ${n_aud}개 — 범위 밖·없는 VM 결과·코드 동일"
else fail "1b: 거절 감사 행 모양(행 ${n_aud:-0}, 어긋남 '${odd}')"; fi
                                     
check "$(emsg "$(as on "$VIEW_M" "{\"$VIEW_K\":\"$VM\"}")")" "$DENY" "1c: on 은 oa1 VM 을 못 본다($VIEW_M)"
set_owner on >/dev/null
R="$(as on "$VIEW_M" "{\"$VIEW_K\":\"$VM\"}")"
[ "$(emsg "$R")" != "$DENY" ] && pass "1d: test 를 on 에게 넘기면 on 이 본다" || fail "1d: on 이 자기 VM 을 못 본다"
check "$(emsg "$(as va "$VIEW_M" "{\"$VIEW_K\":\"$VM\"}")")" "$DENY" "1e: on 소유(테넌트 없음) VM 을 va(ta)는 못 본다"
set_owner oa1 >/dev/null

                                                              
Dv="$(res "$(as ob monitor.vm.diag "{\"vm\":\"$VM\"}")" .vms)"; Dn="$(res "$(as ob monitor.vm.diag "{\"vm\":\"$NOPE\"}")" .vms)"
[ "$Dv" = "[]" ] && [ "$Dv" = "$Dn" ] && pass "2c: monitor.vm.diag 범위 밖 ≡ 없는 이름(vms [])" || fail "2c: monitor.vm.diag vms '$Dv' / '$Dn'"
info "2d: security.event.get — 샌드박스에 VM 대상 보안 이벤트 생산자가 없어 Level 3 에서 본다"

                                                                 
for m in vm.list monitor.fleet; do
    got=""
    for a in oa1 oa2 va ob vn on; do got+="$a=$(mentions_vm "$(as "$a" "$m" '{}')") "; done
    got+="admin=$(mentions_vm "$(uds "$m" '{}')")"
    check "$got" "oa1=yes oa2=yes va=yes ob=no vn=no on=no admin=yes" "3a: $m 계정별 $VM 보임"
done
P="$(as oa1 monitor.processes '{}')"
nvm="$(res "$P" '[.. | objects | select((.type? // .kind? // "") == "vm")] | length')"
check "${nvm:-0}" "0" "3b: monitor.processes 비ADMIN VM 행 0(표본기 error — 닫는 쪽)"

                                                                 
declare -A TOK
for a in oa2 ob va vn; do TOK[$a]="$(login_token "$a" "${PWF[$a]}")"; done
TOK[admin]="$(login_token admin "$ADMIN_PWF")"

                                                                 
R="$(as ob vm.stop "{\"name\":\"$VM\"}")"
check "$(ecode "$R")" "-32006" "5: ob 가 oa1 VM 에 vm.stop → −32006(옛 조작 문구 '$(emsg "$R")')"

                                                                
KEYA="$(res "$(uds auth.apikey.create '{"name":"e2e-key-admin","role":2}')" .api_key | tr -d '"')"
KEY0="$(res "$(uds auth.apikey.create '{"name":"e2e-key-viewer","role":0}')" .api_key | tr -d '"')"
vm_series() { local n; n="$(grep -c 'vm="' "$STATE/body.out" 2>/dev/null)"; echo "${n:-0}"; }
c_d="$(http_code "$BASE_URL/metrics")";                                   n_d="$(vm_series)"
c_a="$(http_code "${XFF[@]}" -H "X-API-Key: $KEYA" "$BASE_URL/metrics")"; n_a="$(vm_series)"
c_0="$(http_code "${XFF[@]}" -H "X-API-Key: $KEY0" "$BASE_URL/metrics")"; n_0="$(vm_series)"
c_v="$(http_code "${XFF[@]}" -H "Authorization: Bearer ${TOK[va]}" "$BASE_URL/metrics")"; n_v="$(vm_series)"
check "$c_d/$c_a/$c_0/$c_v" "200/200/200/200" "6a: /metrics 네 갈래 200"
check "$n_0/$n_v" "0/0" "6b: /metrics 권한 0 키·VIEWER JWT 는 VM 계열 0(지도 error — 닫는 쪽)"
[ "$n_a" = "$n_d" ] && pass "6c: /metrics ADMIN 키 VM 계열 = 직접 로컬($n_d)" || fail "6c: /metrics ADMIN $n_a ≠ 직접 $n_d"
check "$(http_code "$BASE_URL/internal/vms")/$(http_code "${XFF[@]}" -H "X-API-Key: $KEYA" "$BASE_URL/internal/vms")/$(http_code "${XFF[@]}" -H "X-API-Key: $KEY0" "$BASE_URL/internal/vms")/$(http_code "${XFF[@]}" -H "Authorization: Bearer ${TOK[va]}" "$BASE_URL/internal/vms")" \
    "200/200/403/403" "6d: /internal/vms 직접·ADMIN 키 200, 권한 0 키·VIEWER JWT 403"
                                                      
for a in va vn; do iso_curl -s -o /dev/null -X POST "$BASE_URL/auth/token" -H 'Content-Type: application/json' \
    -d "{\"username\":\"$a\",\"password\":\"wrong-password-e2e\"}" >/dev/null 2>&1; done
sleep 1
http_code "${XFF[@]}" -H "Authorization: Bearer ${TOK[va]}" "$BASE_URL/health/recent-errors" >/dev/null
mine="$(jq -c '[.data[]?] | length' "$STATE/body.out" 2>/dev/null)"
others="$(jq -c '[.data[]? | tostring | select(test("\"vn\"|no Authorization"))] | length' "$STATE/body.out" 2>/dev/null)"
http_code "$BASE_URL/health/recent-errors" >/dev/null; all="$(jq -c '[.data[]?] | length' "$STATE/body.out" 2>/dev/null)"
[ "${others:-1}" = "0" ] && [ "${all:-0}" -gt "${mine:-0}" ] \
    && pass "6e: recent-errors VIEWER 는 자기 행만(${mine}행, 직접 로컬 ${all}행)" || fail "6e: recent-errors va ${mine}행·남의 행 ${others}·전체 ${all}"

                                                         
ws_listen() {                                              
    pcv_iso_exec "$(BP)" node -e '
const [url, token, out] = process.argv.slice(1); const fs = require("fs");
const ws = new WebSocket(url);
ws.onopen = () => ws.send(JSON.stringify({ type: "auth", token }));
ws.onmessage = e => fs.appendFileSync(out, String(e.data) + "\n");
setTimeout(() => process.exit(0), 40000);' "ws://127.0.0.1:$PORT/api/v1/ws/events" "$2" "$STATE/ws/$1" >/dev/null 2>&1 &
    WS_PIDS+=("$!")
}
for a in oa2 ob admin; do [ -n "${TOK[$a]}" ] && ws_listen "$a" "${TOK[$a]}"; done
sleep 2

                                                                   
wait_job() { for _ in $(seq 1 40); do s="$(res "$(uds jobs.get "{\"job_id\":\"$1\"}")" .status | tr -d '"')"; case "$s" in completed|failed) return 0 ;; esac; sleep 0.5; done; return 1; }
listed() { res "$(as "$1" jobs.list '{}')" "[.items[]?.job_id] | index(\"$2\") != null"; }
                                                                 
JA="$(res "$(as adma vm.delete '{"vm_id":"pcv-ghost-a"}')" .job_id | tr -d '"')"
[ -n "$JA" ] && wait_job "$JA" || fail "4a: ADMIN Job 접수 실패"
check "$(listed va "$JA")" "false" "4a: ADMIN(ta) 접수 Job 을 va(ta)가 jobs.list 에서 못 본다(리뷰 M1)"
J1="$(res "$(as oa1 vm.delete "{\"vm_id\":\"$VM\"}")" .job_id | tr -d '"')"
[ -n "$J1" ] && wait_job "$J1" || fail "4b: oa1 Job 접수 실패"
check "$(listed oa2 "$J1")" "true" "4b: oa1 Job 을 oa2 가 jobs.list 에서 본다"
check "$(listed ob "$J1")/$(listed vn "$J1")" "false/false" "4c: ob·vn 은 jobs.list 에서 못 본다"
G2="$(as oa2 jobs.get "{\"job_id\":\"$J1\"}")"
[ -z "$(ecode "$G2")" ] && pass "4d: oa2 jobs.get 성공" || fail "4d: oa2 jobs.get $(ecode "$G2")"
for a in ob vn; do
    [ "$(as "$a" jobs.get "{\"job_id\":\"$J1\"}")" = "$(as "$a" jobs.get '{"job_id":"job-00000000"}')" ] \
        && pass "4e: $a jobs.get 범위 밖 ≡ 없는 id" || fail "4e: $a jobs.get 응답이 없는 id 와 다르다"
    [ "$(as "$a" vm.delete.status "{\"name\":\"$VM\"}" | jq -cS 'del(.result.name)')" = \
      "$(as "$a" vm.delete.status "{\"name\":\"$NOPE\"}" | jq -cS 'del(.result.name)')" ] \
        && pass "4f: $a vm.delete.status 범위 밖 ≡ 없는 이름(요청 이름 칸 제외)" || fail "4f: $a vm.delete.status 응답이 다르다"
done
check "$(res "$(as oa2 vm.delete.status "{\"name\":\"$VM\"}")" .job_id | tr -d '"')" "$J1" "4g: oa2 vm.delete.status 가 oa1 Job 을 가리킨다"
uds auth.user.delete '{"username":"oa1"}' >/dev/null
check "$(listed oa2 "$J1")" "true" "4h: 행위자 oa1 계정을 지운 뒤에도 oa2 가 본다"

                                                                 
sleep 3
got_job() { grep -q "\"job.complete\"" "$STATE/ws/$1" 2>/dev/null && grep -q "$J1" "$STATE/ws/$1" 2>/dev/null && echo yes || echo no; }
check "oa2=$(got_job oa2) admin=$(got_job admin) ob=$(got_job ob)" "oa2=yes admin=yes ob=no" "7a: oa1 Job 의 job.complete 수신"
info "7b: healing·anomaly 방송은 샌드박스에 생산자가 없어 단위 시험(/vm_read_filter/ws_*)과 Level 3 에서 본다"

                                                                
OI='Invalid params: owner must be an existing user'
row() { local R; R="$(uds vm.owner.set "$1")"; printf '%s|%s' "$(ecode "$R")" "$(emsg "$R")"; }
check "$(row '{}')" "-32602|Missing required parameter: name" "8a: name 없음"
check "$(row "{\"name\":\"$NOPE\"}")" "-32602|Missing required parameter: owner" "8b: owner 없음"
for o in '"grpc"' '"local-admin"' '"system"' '"-"' '"nobody-e2e"' '5' 'null' '""'; do
    check "$(row "{\"name\":\"$NOPE\",\"owner\":$o}")" "-32602|$OI" "8c: owner $o 거절"
done
check "$(row "{\"name\":\"$NOPE\",\"owner\":\"oa2\",\"expected_owner\":5}")" \
    "-32602|Invalid params: expected_owner must be a string or null" "8d: expected_owner 형식"
check "$(row "{\"name\":\"$NOPE\",\"owner\":\"oa2\"}" | cut -d'|' -f1)" "-32001" "8e: 없는 VM −32001"
check "$(ecode "$(as oa2 vm.owner.set "{\"name\":\"$NOPE\",\"owner\":\"oa2\"}")")" "-32006" "8f: OPERATOR 호출은 정책표 거절"

echo
echo -e "${CYAN}  결과: PASS=$PASS FAIL=$FAIL  mode=$MODE${NC}"
[ "$FAIL" -eq 0 ]
