#!/usr/bin/env bash
                                                                                        
                                                                    
                                                             
                                                                              
                                              
 
     
                                                                    
                                     
     
                                                               
     
                                                           
 
                 
                                                               
                                                                      
                                                             
                                            
 
                
                                                                           
                                                           
                                   

set -Eeuo pipefail

readonly SERVICE="purecvisorsd.service"
readonly EXCLUDED_ADDRESS="192.0.2.100"
readonly VPC_DB="/var/lib/purecvisor/vpc.db"
readonly AUDIT_DB="/var/lib/purecvisor/pcv_audit.db"
readonly TENANT="pcv-vpc-cli-live"
readonly VPC_NAME="pcv-cli-live-nat"
readonly SUBNET_NAME="pcv-cli-live-subnet"
readonly VM_NAME="pcv-vpc-cli-live-vm"
readonly GUEST_NS="pcvcliguest"
readonly GUEST_HOST_VETH="pcvclih0"
readonly GUEST_NS_VETH="pcvclig0"
readonly EXTERNAL_NS="pcvcliext"
readonly EXTERNAL_HOST_VETH="pcvclixh0"
readonly EXTERNAL_NS_VETH="pcvclixn0"
readonly VPC_CIDR="10.254.40.0/24"
readonly GUEST_IP="10.254.40.10"
readonly VPC_GATEWAY="10.254.40.1"
readonly EXTERNAL_CIDR="172.31.254.0/29"
readonly EXTERNAL_HOST_IP="172.31.254.1"
readonly EXTERNAL_NS_IP="172.31.254.2"
readonly PCVCTL="${PCVCTL:-/usr/local/bin/pcvctl}"
readonly INJECT_FAIL_AFTER="${PCV_VPC_CLI_LIVE_FAIL_AFTER:-}"

VPC_ID=""
SUBNET_ID=""
ATTACHMENT_ID=""
BRIDGE_NAME=""
ATTACHMENT_MAC=""
PCV_CLI_LIVE_DIR=""
VM_DEFINED=0
CLEANUP_RUNNING=0
CLEANUP_DONE=0
PASS_COUNT=0
AUDIT_BASE_ID=0
BASE_VPC_COUNT=0
BASE_SUBNET_COUNT=0
BASE_ATTACHMENT_COUNT=0
BASE_PUBLISH_COUNT=0

log()  { printf '[VPC-CLI-LIVE] %s\n' "$*"; }
pass() { PASS_COUNT=$((PASS_COUNT + 1)); log "PASS $PASS_COUNT — $*"; }
die()  { log "FAIL — $*" >&2; exit 1; }

netns_exists()
{
    ip netns list | awk '{print $1}' | grep -Fxq "$1"
}

link_exists()
{
    ip link show "$1" >/dev/null 2>&1
}

                                                                             
                                                      
dnsmasq_for_bridge_exists()
{
    pgrep -a -x dnsmasq 2>/dev/null | grep -Fq "dnsmasq-$1.conf"
}

cli_query()
{
    "$PCVCTL" --no-color --format=json vpc "$@"
}

                                                                  
                                                                    
cli_mutation()
{
    local response
    response="$(cli_query "$@")" || return 1
    printf '%s\n' "$response" >&2
    printf '%s' "$response" | jq -e '
        .result.job_id != null and
        .result.status == "completed" and
        (.result.result | type) == "string"
    ' >/dev/null || return 1
    printf '%s' "$response"
}

job_result()
{
    jq -cer '.result.result | fromjson'
}

cleanup_resources()
{
    local cleanup_rc=0 response
    [[ "$CLEANUP_RUNNING" -eq 0 ]] || return 0
    CLEANUP_RUNNING=1
    set +e
    log "임시 CLI live 자원 역순 cleanup 시작"

    netns_exists "$GUEST_NS" && ip netns delete "$GUEST_NS"
    netns_exists "$EXTERNAL_NS" && ip netns delete "$EXTERNAL_NS"
    link_exists "$GUEST_HOST_VETH" && ip link delete "$GUEST_HOST_VETH"
    link_exists "$EXTERNAL_HOST_VETH" && ip link delete "$EXTERNAL_HOST_VETH"

    if [[ -n "$ATTACHMENT_ID" ]]; then
        response="$(cli_mutation attachment-delete "$ATTACHMENT_ID" --tenant "$TENANT")"
        if [[ $? -eq 0 ]]; then
            ATTACHMENT_ID=""
        else
            log "attachment cleanup 실패 — 재시도용 VM 정의를 보존한다" >&2
            cleanup_rc=1
        fi
    fi

    if [[ "$VM_DEFINED" -eq 1 && -z "$ATTACHMENT_ID" ]]; then
        if virsh undefine "$VM_NAME" >/dev/null; then
            VM_DEFINED=0
        else
            cleanup_rc=1
        fi
    fi

    if [[ -n "$SUBNET_ID" ]]; then
        response="$(cli_mutation subnet-delete "$SUBNET_ID" --tenant "$TENANT")"
        if [[ $? -eq 0 ]]; then
            SUBNET_ID=""
        else
            cleanup_rc=1
        fi
    fi

    if [[ -n "$VPC_ID" ]]; then
        response="$(cli_mutation delete "$VPC_ID" --tenant "$TENANT")"
        if [[ $? -eq 0 ]]; then
            VPC_ID=""
        else
            cleanup_rc=1
        fi
    fi

    if [[ -n "$PCV_CLI_LIVE_DIR" ]]; then
        case "$PCV_CLI_LIVE_DIR" in
            /run/pcv-vpc-cli-live.*)
                rm -rf -- "$PCV_CLI_LIVE_DIR"
                ;;
            *)
                log "예상 밖 임시 경로는 삭제하지 않는다: $PCV_CLI_LIVE_DIR" >&2
                cleanup_rc=1
                ;;
        esac
    fi
    PCV_CLI_LIVE_DIR=""

    CLEANUP_RUNNING=0
    set -e
    return "$cleanup_rc"
}

on_exit()
{
    local exit_code=$?
    trap - EXIT INT TERM
    if [[ "$CLEANUP_DONE" -eq 0 ]]; then
        cleanup_resources || exit_code=1
    fi
    exit "$exit_code"
}

trap on_exit EXIT
trap 'exit 130' INT TERM

[[ "$EUID" -eq 0 ]] || die "root 권한이 필요하다"
[[ "${PCV_VPC_CLI_LIVE:-}" == "1" ]] || die "PCV_VPC_CLI_LIVE=1 명시 opt-in이 필요하다"
[[ -x "$PCVCTL" ]] || die "실행 가능한 pcvctl이 없다: $PCVCTL"
case "$INJECT_FAIL_AFTER" in
    ""|nat) ;;
    *) die "알 수 없는 failure injection stage: $INJECT_FAIL_AFTER" ;;
esac
if ip -o -4 addr show | awk '{print $4}' | grep -qE "^${EXCLUDED_ADDRESS//./\\.}/"; then
    die "제외 노드 $EXCLUDED_ADDRESS 에서는 실행할 수 없다"
fi
systemctl is-active --quiet "$SERVICE" || die "$SERVICE 가 active가 아니다"
[[ -f "$VPC_DB" && -f "$AUDIT_DB" ]] || die "VPC/audit DB가 없다"

for command in awk grep ip jq pgrep ping sqlite3 systemctl virsh; do
    command -v "$command" >/dev/null || die "$command 명령이 없다"
done
[[ "$(sysctl -n net.ipv4.ip_forward)" == "1" ]] || die "net.ipv4.ip_forward=1이 아니다"

BASE_STATUS="$(cli_query status)" || die "초기 vpc.status 호출 실패"
BASE_VPC_COUNT="$(printf '%s' "$BASE_STATUS" | jq -er '.result.vpc_count')"
BASE_SUBNET_COUNT="$(printf '%s' "$BASE_STATUS" | jq -er '.result.subnet_count')"
BASE_ATTACHMENT_COUNT="$(printf '%s' "$BASE_STATUS" | jq -er '.result.attachment_count')"
BASE_PUBLISH_COUNT="$(printf '%s' "$BASE_STATUS" | jq -er '.result.service_publish_count')"
printf '%s' "$BASE_STATUS" | jq -e '.result.healthy == true' >/dev/null \
    || die "초기 VPC 상태가 healthy가 아니다"

TENANT_LIST="$(cli_query list --tenant "$TENANT")" || die "시험 tenant 조회 실패"
printf '%s' "$TENANT_LIST" | jq -e --arg name "$VPC_NAME" \
    '.result | all(.name != $name)' >/dev/null || die "시험 VPC 이름이 이미 존재한다"
virsh dominfo "$VM_NAME" >/dev/null 2>&1 && die "시험 VM 이름이 이미 존재한다"
netns_exists "$GUEST_NS" && die "시험 namespace가 이미 존재한다: $GUEST_NS"
netns_exists "$EXTERNAL_NS" && die "시험 namespace가 이미 존재한다: $EXTERNAL_NS"
link_exists "$GUEST_HOST_VETH" && die "시험 veth가 이미 존재한다: $GUEST_HOST_VETH"
link_exists "$EXTERNAL_HOST_VETH" && die "시험 veth가 이미 존재한다: $EXTERNAL_HOST_VETH"
ip route show | grep -Fq "$VPC_CIDR" && die "시험 VPC CIDR route가 이미 존재한다"
ip route show | grep -Fq "$EXTERNAL_CIDR" && die "시험 외부 CIDR route가 이미 존재한다"
[[ "$(sqlite3 "$VPC_DB" "SELECT count(*) FROM subnets WHERE cidr='$VPC_CIDR';")" == "0" ]] \
    || die "시험 VPC CIDR이 desired state에 이미 존재한다"
AUDIT_BASE_ID="$(sqlite3 "$AUDIT_DB" 'SELECT COALESCE(MAX(id), 0) FROM audit_log;')"
pass "서비스·CLI·제외 노드·임시 이름·CIDR preflight"

PCV_CLI_LIVE_DIR="$(mktemp -d /run/pcv-vpc-cli-live.XXXXXX)"
cat >"$PCV_CLI_LIVE_DIR/domain.xml" <<EOF
<domain type='kvm'>
  <name>$VM_NAME</name>
  <memory unit='MiB'>64</memory>
  <vcpu>1</vcpu>
  <os><type arch='x86_64' machine='pc-q35-10.2'>hvm</type></os>
  <features><acpi/><apic/></features>
  <devices>
    <emulator>/usr/bin/qemu-system-x86_64</emulator>
    <memballoon model='none'/>
  </devices>
</domain>
EOF
virsh define "$PCV_CLI_LIVE_DIR/domain.xml" >/dev/null
VM_DEFINED=1

CREATE_RESPONSE="$(cli_mutation create "$VPC_NAME" --tenant "$TENANT" --egress nat)" \
    || die "CLI VPC 생성 실패"
CREATE_RESULT="$(printf '%s' "$CREATE_RESPONSE" | job_result)"
VPC_ID="$(printf '%s' "$CREATE_RESULT" | jq -er '.id')"
REVISION="$(printf '%s' "$CREATE_RESULT" | jq -er '.revision')"
[[ "$(printf '%s' "$CREATE_RESULT" | jq -er '.state')" == "ACTIVE" ]] \
    || die "생성 VPC가 ACTIVE가 아니다"

SUBNET_RESPONSE="$(cli_mutation subnet-create "$VPC_ID" "$SUBNET_NAME" \
    --tenant "$TENANT" --cidr "$VPC_CIDR" --mtu 1500 --revision "$REVISION")" \
    || die "CLI subnet 생성 실패"
SUBNET_RESULT="$(printf '%s' "$SUBNET_RESPONSE" | job_result)"
SUBNET_ID="$(printf '%s' "$SUBNET_RESULT" | jq -er '.id')"
BRIDGE_NAME="$(printf '%s' "$SUBNET_RESULT" | jq -er '.bridge_name')"

ATTACHMENT_RESPONSE="$(cli_mutation attachment-create "$SUBNET_ID" "$VM_NAME" \
    --tenant "$TENANT" --ip "$GUEST_IP")" || die "CLI attachment 생성 실패"
ATTACHMENT_RESULT="$(printf '%s' "$ATTACHMENT_RESPONSE" | job_result)"
ATTACHMENT_ID="$(printf '%s' "$ATTACHMENT_RESULT" | jq -er '.id')"
ATTACHMENT_MAC="$(printf '%s' "$ATTACHMENT_RESULT" | jq -er '.mac_address')"
[[ "$(printf '%s' "$ATTACHMENT_RESULT" | jq -er '.ip_address')" == "$GUEST_IP" ]] \
    || die "attachment IP가 요청값과 다르다"
ip link show "$BRIDGE_NAME" >/dev/null || die "managed bridge가 없다: $BRIDGE_NAME"
dnsmasq_for_bridge_exists "$BRIDGE_NAME" \
    || die "managed dnsmasq가 없다: $BRIDGE_NAME"
pass "설치본 CLI VPC·subnet·attachment terminal 생성과 실제 bridge/dnsmasq"

ip netns add "$GUEST_NS"
ip link add "$GUEST_HOST_VETH" type veth peer name "$GUEST_NS_VETH"
ip link set "$GUEST_NS_VETH" netns "$GUEST_NS"
ip link set "$GUEST_HOST_VETH" master "$BRIDGE_NAME"
ip link set "$GUEST_HOST_VETH" up
ip -n "$GUEST_NS" link set lo up
ip -n "$GUEST_NS" link set "$GUEST_NS_VETH" address "$ATTACHMENT_MAC"
ip -n "$GUEST_NS" addr add "$GUEST_IP/24" dev "$GUEST_NS_VETH"
ip -n "$GUEST_NS" link set "$GUEST_NS_VETH" up
ip -n "$GUEST_NS" route add default via "$VPC_GATEWAY"

ip netns add "$EXTERNAL_NS"
ip link add "$EXTERNAL_HOST_VETH" type veth peer name "$EXTERNAL_NS_VETH"
ip link set "$EXTERNAL_NS_VETH" netns "$EXTERNAL_NS"
ip addr add "$EXTERNAL_HOST_IP/29" dev "$EXTERNAL_HOST_VETH"
ip link set "$EXTERNAL_HOST_VETH" up
ip -n "$EXTERNAL_NS" link set lo up
ip -n "$EXTERNAL_NS" addr add "$EXTERNAL_NS_IP/29" dev "$EXTERNAL_NS_VETH"
ip -n "$EXTERNAL_NS" link set "$EXTERNAL_NS_VETH" up
ip -n "$EXTERNAL_NS" route add default via "$EXTERNAL_HOST_IP"

NAT_PING="$(ip netns exec "$GUEST_NS" ping -c 3 -W 1 "$EXTERNAL_NS_IP")" \
    || die "NAT VPC outbound/reply ping 실패"
printf '%s\n' "$NAT_PING"
pass "NAT VPC 동일 패킷 3/3 성공"
[[ "$INJECT_FAIL_AFTER" != "nat" ]] || die "테스트 전용 NAT 이후 실패 주입"

GET_RESPONSE="$(cli_query get "$VPC_ID" --tenant "$TENANT")" || die "VPC 상세 조회 실패"
REVISION="$(printf '%s' "$GET_RESPONSE" | jq -er '.result.revision')"
[[ "$(printf '%s' "$GET_RESPONSE" | jq -er '.result.egress_mode')" == "nat" ]] \
    || die "차단 전 egress mode가 nat가 아니다"

EGRESS_RESPONSE="$(cli_mutation egress-set "$VPC_ID" --tenant "$TENANT" \
    --egress isolated --revision "$REVISION")" || die "CLI isolated 전환 실패"
EGRESS_RESULT="$(printf '%s' "$EGRESS_RESPONSE" | job_result)"
[[ "$(printf '%s' "$EGRESS_RESULT" | jq -er '.egress_mode')" == "isolated" ]] \
    || die "worker 결과가 isolated가 아니다"

set +e
ISOLATED_PING="$(ip netns exec "$GUEST_NS" ping -c 3 -W 1 "$EXTERNAL_NS_IP" 2>&1)"
ISOLATED_RC=$?
set -e
printf '%s\n' "$ISOLATED_PING"
[[ "$ISOLATED_RC" -ne 0 ]] || die "isolated 전환 뒤 동일 외부 ping이 통과했다"
ip netns exec "$GUEST_NS" ping -c 2 -W 1 "$VPC_GATEWAY" >/dev/null \
    || die "isolated 전환 뒤 VPC gateway까지 끊겼다"
pass "CLI isolated 전환 뒤 동일 외부 패킷 차단, VPC gateway 유지"

cleanup_resources || die "CLI live 자원 cleanup 실패"
CLEANUP_DONE=1

FINAL_STATUS="$(cli_query status)" || die "최종 vpc.status 호출 실패"
[[ "$(printf '%s' "$FINAL_STATUS" | jq -er '.result.vpc_count')" == "$BASE_VPC_COUNT" ]] \
    || die "최종 VPC 수가 기준선과 다르다"
[[ "$(printf '%s' "$FINAL_STATUS" | jq -er '.result.subnet_count')" == "$BASE_SUBNET_COUNT" ]] \
    || die "최종 subnet 수가 기준선과 다르다"
[[ "$(printf '%s' "$FINAL_STATUS" | jq -er '.result.attachment_count')" == "$BASE_ATTACHMENT_COUNT" ]] \
    || die "최종 attachment 수가 기준선과 다르다"
[[ "$(printf '%s' "$FINAL_STATUS" | jq -er '.result.service_publish_count')" == "$BASE_PUBLISH_COUNT" ]] \
    || die "최종 publish 수가 기준선과 다르다"
printf '%s' "$FINAL_STATUS" | jq -e \
    '.result.healthy == true and .result.reconcile_required == false' >/dev/null \
    || die "최종 VPC 상태가 healthy/reconciled가 아니다"

FINAL_TENANT_LIST="$(cli_query list --tenant "$TENANT")" || die "최종 tenant 조회 실패"
printf '%s' "$FINAL_TENANT_LIST" | jq -e '.result | length == 0' >/dev/null \
    || die "시험 tenant VPC가 남았다"
virsh dominfo "$VM_NAME" >/dev/null 2>&1 && die "임시 VM이 남았다"
netns_exists "$GUEST_NS" && die "guest namespace가 남았다"
netns_exists "$EXTERNAL_NS" && die "external namespace가 남았다"
link_exists "$GUEST_HOST_VETH" && die "guest veth가 남았다"
link_exists "$EXTERNAL_HOST_VETH" && die "external veth가 남았다"
link_exists "$BRIDGE_NAME" && die "managed bridge가 남았다"
dnsmasq_for_bridge_exists "$BRIDGE_NAME" \
    && die "managed dnsmasq가 남았다"

AUDIT_OK="$(sqlite3 "$AUDIT_DB" "SELECT count(*) FROM audit_log WHERE id > $AUDIT_BASE_ID AND target LIKE '$TENANT/%' AND result='ok';")"
AUDIT_FAIL="$(sqlite3 "$AUDIT_DB" "SELECT count(*) FROM audit_log WHERE id > $AUDIT_BASE_ID AND target LIKE '$TENANT/%' AND result!='ok';")"
[[ "$AUDIT_OK" -ge 7 && "$AUDIT_FAIL" -eq 0 ]] \
    || die "worker audit가 부족하거나 실패를 포함한다: ok=$AUDIT_OK fail=$AUDIT_FAIL"
systemctl is-active --quiet "$SERVICE" || die "cleanup 뒤 daemon이 active가 아니다"
pass "CLI 역순 삭제, audit $AUDIT_OK건 성공, 기준선 복원과 임시 자원 무잔류"

log "PASS — privileged Local VPC CLI live 효과 게이트 $PASS_COUNT개 완료"
