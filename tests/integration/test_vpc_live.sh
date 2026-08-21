#!/usr/bin/env bash
                                                                             
                                                                      
                                                                
                                                                             
 
     
                                                                                    
                                                            
 
                 
                                                                         
                                                          
                                                     
                                                         
                                                                
                           
 
                
                                                                    
                                                         
                                                          
 
     
                                                                   

set -Eeuo pipefail

readonly SERVICE="purecvisorsd.service"
readonly SOCKET="/var/run/purecvisor/daemon.sock"
readonly VPC_DB="/var/lib/purecvisor/vpc.db"
readonly AUDIT_DB="/var/lib/purecvisor/pcv_audit.db"
readonly EXCLUDED_ADDRESS="192.0.2.100"
readonly TENANT="pcv-vpc-live"
readonly VM_A="pcv-vpc-live-a"
readonly VM_B="pcv-vpc-live-b"
readonly SG_NAME="pcv-vpc-live-sg"
readonly SOURCE_NS="pcv-vpc-live-src"
readonly SOURCE_VETH_HOST="pcvvpchost0"
readonly SOURCE_VETH_NS="pcvvpcsrc0"
readonly SOURCE_HOST_IP="172.31.255.1"
readonly SOURCE_NS_IP="172.31.255.2"
readonly SOURCE_DENIED_IP="172.31.255.3"
readonly SOURCE_PREFIX="172.31.255.0/29"
readonly VPC_A_SUBNET_1="10.253.10.0/24"
readonly VPC_A_SUBNET_2="10.253.11.0/24"
readonly VPC_B_SUBNET="10.253.20.0/24"
readonly VPC_ISOLATED_SUBNET="10.253.30.0/24"
readonly PUBLISH_PORT="18443"
readonly TARGET_PORT="8080"

TMPDIR_LIVE=""
INITRAMFS=""
KERNEL_COPY=""
VPC_A_ID=""
VPC_B_ID=""
VPC_ISOLATED_ID=""
SUBNET_A1_ID=""
SUBNET_A2_ID=""
SUBNET_B_ID=""
SUBNET_ISOLATED_ID=""
BRIDGE_A1=""
BRIDGE_A2=""
BRIDGE_B=""
BRIDGE_ISOLATED=""
ATTACHMENT_A_ID=""
ATTACHMENT_B_ID=""
ATTACHMENT_A_IP=""
ATTACHMENT_B_IP=""
ATTACHMENT_A_MAC=""
PUBLISH_ID=""
SG_CREATED=0
SG_BOUND=0
SG_B_BOUND=0
SOURCE_NS_CREATED=0
VM_A_DEFINED=0
VM_B_DEFINED=0
VM_A_RUNNING=0
VM_B_RUNNING=0
PASS_COUNT=0
CLEANUP_RUNNING=0
AUDIT_BASE_ID=0

log()  { printf '[VPC-LIVE] %s\n' "$*"; }
pass() { PASS_COUNT=$((PASS_COUNT + 1)); log "PASS $PASS_COUNT — $*"; }
die()  { log "FAIL — $*" >&2; exit 1; }

rpc()
{
    local payload="$1"
    python3 - "$SOCKET" 3< <(printf '%s' "$payload") <<'PY'
import socket
import sys

path = sys.argv[1]
payload = open(3, "rb", closefd=False).read()
client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
client.settimeout(10)
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
    if key:
        value = value[int(key)] if isinstance(value, list) else value[key]
if isinstance(value, bool):
    print("true" if value else "false")
elif value is None:
    print("")
elif isinstance(value, (dict, list)):
    print(json.dumps(value, separators=(",", ":")))
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

vpc_xml_fingerprint()
{
    local vm="$1"
    local mac="$2"
    local attachment_id="$3"
    virsh dumpxml --inactive "$vm" | python3 -c '
import json, sys
from xml.etree import ElementTree as ET

mac, attachment_id = sys.argv[1:]
root = ET.fromstring(sys.stdin.read())
local = lambda tag: tag.rsplit("}", 1)[-1].rsplit(":", 1)[-1]
interfaces = []
attachments = []
for node in root.iter():
    if local(node.tag) == "interface":
        children = {local(child.tag): child for child in node}
        mac_node = children.get("mac")
        if mac_node is not None and mac_node.get("address", "").lower() == mac.lower():
            interfaces.append({
                "source": children.get("source").get("bridge") if children.get("source") is not None else None,
                "model": children.get("model").get("type") if children.get("model") is not None else None,
                "driver": children.get("driver").get("name") if children.get("driver") is not None else None,
                "mtu": children.get("mtu").get("size") if children.get("mtu") is not None else None,
            })
    elif local(node.tag) == "vpc":
        for child in node:
            if local(child.tag) == "attachment" and child.get("id") == attachment_id:
                attachments.append({"vpc": node.get("id"), **dict(sorted(child.attrib.items()))})
print(json.dumps({"interfaces": interfaces, "attachments": attachments}, sort_keys=True))
' "$mac" "$attachment_id"
}

wait_ready()
{
    local attempt
    for attempt in $(seq 1 100); do
        if systemctl is-active --quiet "$SERVICE" && [[ -S "$SOCKET" ]] &&
           rpc '{"jsonrpc":"2.0","method":"vpc.status","params":{},"id":"ready"}' \
               >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.2
    done
    return 1
}

wait_job()
{
    local job_id="$1"
    local response status attempt
    for attempt in $(seq 1 200); do
        response="$(rpc "{\"jsonrpc\":\"2.0\",\"method\":\"jobs.get\",\"params\":{\"job_id\":\"$job_id\"},\"id\":\"job\"}")"
        status="$(field "$response" result.status 2>/dev/null || true)"
        case "$status" in
            completed)
                printf '%s' "$response"
                return 0
                ;;
            failed|cancelled)
                log "Job $job_id terminal response: $response" >&2
                return 1
                ;;
        esac
        sleep 0.1
    done
    log "Job $job_id timeout" >&2
    return 1
}

rpc_mutation()
{
    local payload="$1"
    local response job_id
    response="$(rpc "$payload")"
    job_id="$(field "$response" result.job_id 2>/dev/null || true)"
    [[ -n "$job_id" ]] || {
        log "mutation accepted 응답이 아니다: $response" >&2
        return 1
    }
    wait_job "$job_id"
}

job_result()
{
    local job="$1"
    local encoded
    encoded="$(field "$job" result.result)" || return 1
    printf '%s' "$encoded" | python3 -c '
import json, sys
value = json.load(sys.stdin)
print(json.dumps(value, separators=(",", ":")))
'
}

cleanup_mutation()
{
    local payload="$1"
    rpc_mutation "$payload" >/dev/null 2>&1
}

guest_run()
{
    local vm="$1"
    local command="$2"
    local console_socket="$TMPDIR_LIVE/$vm.console.sock"
    local output
    [[ -S "$console_socket" ]] || return 1
    output="$({
        sleep 0.2
        printf '%s\n' "$command"
        printf '%s\n' 'pcv_rc=$?' 'echo __PCV_RC_${pcv_rc}__'
                                                                     
        sleep 3
    } | timeout 5 socat - "UNIX-CONNECT:$console_socket" 2>/dev/null || true)"
    printf '%s\n' "$output"
    [[ "$output" == *"__PCV_RC_0__"* ]]
}

wait_guest()
{
    local vm="$1"
    local expected_ip="$2"
    local attempt
    for attempt in $(seq 1 100); do
        if guest_run "$vm" \
            "ip -4 addr show dev eth0 | grep -q 'inet $expected_ip/24'" \
            >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.2
    done
    log "$vm 준비 진단: expected_ip=$expected_ip" >&2
    guest_run "$vm" "ip -4 addr show dev eth0" >&2 || true
    return 1
}

cleanup()
{
    local exit_code=$?
    local undefine_vm_a=1
    local undefine_vm_b=1
    [[ "$CLEANUP_RUNNING" -eq 0 ]] || return
    CLEANUP_RUNNING=1
    set +e
    log "임시 Local VPC 자원 정리 시작"

    [[ "$VM_A_RUNNING" -eq 1 ]] && virsh destroy "$VM_A" >/dev/null 2>&1
    [[ "$VM_B_RUNNING" -eq 1 ]] && virsh destroy "$VM_B" >/dev/null 2>&1
    VM_A_RUNNING=0
    VM_B_RUNNING=0

    if [[ -n "$PUBLISH_ID" ]]; then
        cleanup_mutation "{\"jsonrpc\":\"2.0\",\"method\":\"vpc.service.unpublish\",\"params\":{\"tenant\":\"$TENANT\",\"publish_id\":\"$PUBLISH_ID\"},\"id\":\"cleanup-publish\"}" \
            || exit_code=1
    fi
    PUBLISH_ID=""

    if [[ "$SG_BOUND" -eq 1 ]]; then
        rpc "{\"jsonrpc\":\"2.0\",\"method\":\"security_group.detach\",\"params\":{\"vm\":\"$VM_A\",\"name\":\"$SG_NAME\"},\"id\":\"cleanup-sg-detach\"}" >/dev/null 2>&1
    fi
    SG_BOUND=0
    if [[ "$SG_B_BOUND" -eq 1 ]]; then
        rpc "{\"jsonrpc\":\"2.0\",\"method\":\"security_group.detach\",\"params\":{\"vm\":\"$VM_B\",\"name\":\"$SG_NAME\"},\"id\":\"cleanup-sg-detach-b\"}" >/dev/null 2>&1
    fi
    SG_B_BOUND=0

    if [[ -n "$ATTACHMENT_A_ID" ]]; then
        if ! cleanup_mutation "{\"jsonrpc\":\"2.0\",\"method\":\"vpc.attachment.delete\",\"params\":{\"tenant\":\"$TENANT\",\"attachment_id\":\"$ATTACHMENT_A_ID\"},\"id\":\"cleanup-attachment-a\"}"; then
            log "attachment A cleanup 실패 — 재시도용 VM 정의를 보존한다" >&2
            undefine_vm_a=0
            exit_code=1
        fi
    fi
    if [[ -n "$ATTACHMENT_B_ID" ]]; then
        if ! cleanup_mutation "{\"jsonrpc\":\"2.0\",\"method\":\"vpc.attachment.delete\",\"params\":{\"tenant\":\"$TENANT\",\"attachment_id\":\"$ATTACHMENT_B_ID\"},\"id\":\"cleanup-attachment-b\"}"; then
            log "attachment B cleanup 실패 — 재시도용 VM 정의를 보존한다" >&2
            undefine_vm_b=0
            exit_code=1
        fi
    fi
    ATTACHMENT_A_ID=""
    ATTACHMENT_B_ID=""

    if [[ "$VM_A_DEFINED" -eq 1 && "$undefine_vm_a" -eq 1 ]]; then
        virsh undefine "$VM_A" --nvram >/dev/null 2>&1
        virsh undefine "$VM_A" >/dev/null 2>&1
        VM_A_DEFINED=0
    fi
    if [[ "$VM_B_DEFINED" -eq 1 && "$undefine_vm_b" -eq 1 ]]; then
        virsh undefine "$VM_B" --nvram >/dev/null 2>&1
        virsh undefine "$VM_B" >/dev/null 2>&1
        VM_B_DEFINED=0
    fi

    for subnet_id in "$SUBNET_A1_ID" "$SUBNET_A2_ID" "$SUBNET_B_ID" "$SUBNET_ISOLATED_ID"; do
        [[ -n "$subnet_id" ]] || continue
        cleanup_mutation "{\"jsonrpc\":\"2.0\",\"method\":\"vpc.subnet.delete\",\"params\":{\"tenant\":\"$TENANT\",\"subnet_id\":\"$subnet_id\"},\"id\":\"cleanup-subnet\"}" \
            || exit_code=1
    done
    SUBNET_A1_ID=""
    SUBNET_A2_ID=""
    SUBNET_B_ID=""
    SUBNET_ISOLATED_ID=""

    for vpc_id in "$VPC_A_ID" "$VPC_B_ID" "$VPC_ISOLATED_ID"; do
        [[ -n "$vpc_id" ]] || continue
        cleanup_mutation "{\"jsonrpc\":\"2.0\",\"method\":\"vpc.delete\",\"params\":{\"tenant\":\"$TENANT\",\"vpc_id\":\"$vpc_id\"},\"id\":\"cleanup-vpc\"}" \
            || exit_code=1
    done
    VPC_A_ID=""
    VPC_B_ID=""
    VPC_ISOLATED_ID=""

    if [[ "$SG_CREATED" -eq 1 ]]; then
        rpc "{\"jsonrpc\":\"2.0\",\"method\":\"security_group.delete\",\"params\":{\"name\":\"$SG_NAME\"},\"id\":\"cleanup-sg\"}" >/dev/null 2>&1
    fi
    SG_CREATED=0

    if [[ "$SOURCE_NS_CREATED" -eq 1 ]]; then
        ip netns delete "$SOURCE_NS" >/dev/null 2>&1
        ip link delete "$SOURCE_VETH_HOST" >/dev/null 2>&1
    fi
    SOURCE_NS_CREATED=0

    [[ -n "$TMPDIR_LIVE" && -d "$TMPDIR_LIVE" ]] && rm -rf -- "$TMPDIR_LIVE"

    if [[ -S "$SOCKET" ]]; then
        final_status="$(rpc '{"jsonrpc":"2.0","method":"vpc.status","params":{},"id":"cleanup-status"}' 2>/dev/null)"
        if [[ -n "$final_status" ]] &&
           [[ "$(field "$final_status" result.vpc_count 2>/dev/null)" == "0" ]] &&
           [[ "$(field "$final_status" result.subnet_count 2>/dev/null)" == "0" ]] &&
           [[ "$(field "$final_status" result.attachment_count 2>/dev/null)" == "0" ]] &&
           [[ "$(field "$final_status" result.service_publish_count 2>/dev/null)" == "0" ]]; then
            log "cleanup 확인: VPC DB 객체 0"
        else
            log "cleanup 불완전: ${final_status:-vpc.status unavailable}" >&2
            exit_code=1
        fi
    fi
    CLEANUP_RUNNING=0
    return "$exit_code"
}

trap cleanup EXIT INT TERM

[[ "$EUID" -eq 0 ]] || die "root 권한이 필요하다"
[[ "${PCV_VPC_LIVE:-}" == "1" ]] || die "PCV_VPC_LIVE=1 명시 승인이 필요하다"
if ip -o -4 addr show 2>/dev/null | awk '{print $4}' \
    | grep -qE "^${EXCLUDED_ADDRESS//./\\.}/"; then
    die "제외 노드 $EXCLUDED_ADDRESS 에서는 실행할 수 없다"
fi
systemctl is-active --quiet "$SERVICE" || die "$SERVICE 가 active가 아니다"
wait_ready || die "라이브 UDS가 응답하지 않는다"
[[ -f "$VPC_DB" && -f "$AUDIT_DB" ]] || die "VPC/audit DB가 없다"
AUDIT_BASE_ID="$(sqlite3 "$AUDIT_DB" 'SELECT COALESCE(MAX(id), 0) FROM audit_log;')"

for command in python3 jq sqlite3 nft ip virsh cpio gzip busybox timeout file socat; do
    command -v "$command" >/dev/null || die "$command 명령이 없다"
done
[[ "$(file -b /usr/bin/busybox)" == *"statically linked"* ]] \
    || die "/usr/bin/busybox가 정적 실행 파일이 아니다"

BASE_STATUS="$(rpc '{"jsonrpc":"2.0","method":"vpc.status","params":{},"id":"base"}')"
assert_field "$BASE_STATUS" result.vpc_count 0 "초기 VPC 수"
assert_field "$BASE_STATUS" result.subnet_count 0 "초기 subnet 수"
assert_field "$BASE_STATUS" result.attachment_count 0 "초기 attachment 수"
assert_field "$BASE_STATUS" result.service_publish_count 0 "초기 publish 수"

for vm in "$VM_A" "$VM_B"; do
    virsh dominfo "$vm" >/dev/null 2>&1 && die "임시 VM 이름이 이미 존재한다: $vm"
done
ip netns list | awk '{print $1}' | grep -Fxq "$SOURCE_NS" \
    && die "임시 network namespace가 이미 존재한다: $SOURCE_NS"
ip link show "$SOURCE_VETH_HOST" >/dev/null 2>&1 \
    && die "임시 veth가 이미 존재한다: $SOURCE_VETH_HOST"
rpc '{"jsonrpc":"2.0","method":"security_group.list","params":{},"id":"sg-base"}' \
    | jq -e --arg name "$SG_NAME" '.result | all(.name != $name)' >/dev/null \
    || die "임시 Security Group 이름이 이미 존재한다: $SG_NAME"
pass "빈 VPC 기준선과 임시 이름 충돌 없음"

mkdir -p /var/lib/libvirt/boot
TMPDIR_LIVE="$(mktemp -d /var/lib/libvirt/boot/pcv-vpc-live.XXXXXX)"
chmod 0755 "$TMPDIR_LIVE"
mkdir -p "$TMPDIR_LIVE/rootfs"/{bin,dev,etc/udhcpc,proc,sys,run,tmp}
cp /usr/bin/busybox "$TMPDIR_LIVE/rootfs/bin/busybox"
for applet in sh mount ip udhcpc nc sleep poweroff grep; do
    ln -s busybox "$TMPDIR_LIVE/rootfs/bin/$applet"
done

cat >"$TMPDIR_LIVE/rootfs/etc/udhcpc/default.script" <<'GUEST_DHCP'
#!/bin/sh
case "$1" in
    deconfig)
        /bin/ip addr flush dev "$interface"
        ;;
    bound|renew)
        /bin/ip addr flush dev "$interface"
        /bin/ip addr add "$ip/24" dev "$interface"
        if [ -n "$router" ]; then
            /bin/ip route replace default via "${router%% *}" dev "$interface"
        fi
        ;;
esac
GUEST_DHCP

cat >"$TMPDIR_LIVE/rootfs/init" <<'GUEST_INIT'
#!/bin/sh
/bin/mount -t proc proc /proc
/bin/mount -t sysfs sysfs /sys
/bin/mount -t devtmpfs devtmpfs /dev
/bin/ip link set lo up
/bin/ip link set eth0 up
/bin/udhcpc -i eth0 -n -q -t 20 -T 1
/bin/nc -ll -p 10022 -e /bin/sh &
/bin/nc -ll -p 8080 -e /bin/reply &
echo "pcv-vpc-live guest ready"
exec /bin/sh
GUEST_INIT
cat >"$TMPDIR_LIVE/rootfs/bin/reply" <<'GUEST_REPLY'
#!/bin/sh
echo published
GUEST_REPLY
chmod 0755 "$TMPDIR_LIVE/rootfs/init"
chmod 0755 "$TMPDIR_LIVE/rootfs/bin/reply"
chmod 0755 "$TMPDIR_LIVE/rootfs/etc/udhcpc/default.script"
INITRAMFS="$TMPDIR_LIVE/pcv-vpc-live-initramfs.gz"
(cd "$TMPDIR_LIVE/rootfs" && find . -print0 | cpio --null -ov --format=newc 2>/dev/null) \
    | gzip -9 >"$INITRAMFS"
KERNEL_COPY="$TMPDIR_LIVE/vmlinuz"
cp "/boot/vmlinuz-$(uname -r)" "$KERNEL_COPY"
chmod 0644 "$INITRAMFS" "$KERNEL_COPY"

define_guest()
{
    local vm="$1"
    local xml="$TMPDIR_LIVE/$vm.xml"
    local uuid
    uuid="$(cat /proc/sys/kernel/random/uuid)"
    cat >"$xml" <<EOF
<domain type='kvm'>
  <name>$vm</name>
  <uuid>$uuid</uuid>
  <memory unit='MiB'>256</memory>
  <vcpu>1</vcpu>
  <os>
    <type arch='x86_64' machine='pc-q35-10.2'>hvm</type>
    <kernel>$KERNEL_COPY</kernel>
    <initrd>$INITRAMFS</initrd>
    <cmdline>console=ttyS0 rdinit=/init</cmdline>
  </os>
  <features><acpi/><apic/></features>
  <cpu mode='host-passthrough' check='none'/>
  <on_poweroff>destroy</on_poweroff>
  <on_reboot>destroy</on_reboot>
  <on_crash>destroy</on_crash>
  <devices>
    <emulator>/usr/bin/qemu-system-x86_64</emulator>
    <serial type='unix'>
      <source mode='bind' path='$TMPDIR_LIVE/$vm.console.sock'/>
      <target type='isa-serial' port='0'/>
    </serial>
    <console type='unix'>
      <source mode='bind' path='$TMPDIR_LIVE/$vm.console.sock'/>
      <target type='serial' port='0'/>
    </console>
    <memballoon model='none'/>
  </devices>
</domain>
EOF
    virsh define "$xml" >/dev/null
}

define_guest "$VM_A"
VM_A_DEFINED=1
define_guest "$VM_B"
VM_B_DEFINED=1
pass "독립 initramfs 임시 VM 두 대 정의"

create_vpc()
{
    local name="$1"
    local mode="$2"
    local subnet_name="${3:-}"
    local subnet_cidr="${4:-}"
    local job result
    local subnet_json=""
    if [[ -n "$subnet_name" && -n "$subnet_cidr" ]]; then
        subnet_json=",\"subnet_name\":\"$subnet_name\",\"subnet_cidr\":\"$subnet_cidr\",\"subnet_mtu\":1500"
    fi
    job="$(rpc_mutation "{\"jsonrpc\":\"2.0\",\"method\":\"vpc.create\",\"params\":{\"tenant\":\"$TENANT\",\"name\":\"$name\",\"egress_mode\":\"$mode\"$subnet_json},\"id\":\"create-vpc\"}")"
    result="$(job_result "$job")"
    printf '%s\t%s\t%s\t%s\n' "$(field "$result" id)" "$(field "$result" revision)" \
        "$(field "$result" subnet.id 2>/dev/null || true)" \
        "$(field "$result" subnet.bridge_name 2>/dev/null || true)"
}

create_subnet()
{
    local vpc_id="$1"
    local name="$2"
    local cidr="$3"
    local revision="$4"
    local job result
    job="$(rpc_mutation "{\"jsonrpc\":\"2.0\",\"method\":\"vpc.subnet.create\",\"params\":{\"tenant\":\"$TENANT\",\"vpc_id\":\"$vpc_id\",\"name\":\"$name\",\"cidr\":\"$cidr\",\"mtu\":1500,\"expected_revision\":$revision},\"id\":\"create-subnet\"}")"
    result="$(job_result "$job")"
    printf '%s\t%s\t%s\n' "$(field "$result" id)" "$(field "$result" bridge_name)" \
        "$(field "$result" revision)"
}

PARTIAL_CREATE="$(rpc "{\"jsonrpc\":\"2.0\",\"method\":\"vpc.create\",\"params\":{\"tenant\":\"$TENANT\",\"name\":\"pcv-live-partial\",\"egress_mode\":\"nat\",\"subnet_name\":\"missing-cidr\"},\"id\":\"partial-create\"}")"
[[ -n "$(field "$PARTIAL_CREATE" error.message 2>/dev/null || true)" ]] \
    || die "부분 첫 subnet payload가 동기 거부되지 않았다: $PARTIAL_CREATE"
PARTIAL_STATUS="$(rpc '{"jsonrpc":"2.0","method":"vpc.status","params":{},"id":"partial-status"}')"
assert_field "$PARTIAL_STATUS" result.vpc_count 0 "부분 payload 뒤 VPC 수"
assert_field "$PARTIAL_STATUS" result.subnet_count 0 "부분 payload 뒤 subnet 수"
pass "첫 subnet all-or-none 입력을 actual mutation 전에 거부"

IFS=$'\t' read -r VPC_A_ID REV_A SUBNET_A1_ID BRIDGE_A1 \
    < <(create_vpc "pcv-live-nat-a" nat "a-web" "$VPC_A_SUBNET_1")
IFS=$'\t' read -r SUBNET_A2_ID BRIDGE_A2 REV_A < <(create_subnet "$VPC_A_ID" "a-db" "$VPC_A_SUBNET_2" "$REV_A")
IFS=$'\t' read -r VPC_B_ID REV_B SUBNET_B_ID BRIDGE_B \
    < <(create_vpc "pcv-live-nat-b" nat "b-web" "$VPC_B_SUBNET")
IFS=$'\t' read -r VPC_ISOLATED_ID REV_ISOLATED SUBNET_ISOLATED_ID BRIDGE_ISOLATED \
    < <(create_vpc "pcv-live-isolated" isolated "isolated" "$VPC_ISOLATED_SUBNET")

for bridge in "$BRIDGE_A1" "$BRIDGE_A2" "$BRIDGE_B" "$BRIDGE_ISOLATED"; do
    ip link show "$bridge" >/dev/null || die "managed bridge가 없다: $bridge"
    pgrep -a -x dnsmasq | grep -Fq "dnsmasq-$bridge.conf" \
        || die "managed dnsmasq가 없다: $bridge"
done
pass "VPC+첫 subnet 단일 Job 3건과 추가 subnet의 bridge/gateway/dnsmasq 실제 생성"

OVERLAP="$(rpc "{\"jsonrpc\":\"2.0\",\"method\":\"vpc.subnet.create\",\"params\":{\"tenant\":\"$TENANT\",\"vpc_id\":\"$VPC_B_ID\",\"name\":\"overlap\",\"cidr\":\"$VPC_A_SUBNET_1\",\"mtu\":1500,\"expected_revision\":$REV_B},\"id\":\"overlap\"}")"
OVERLAP_JOB="$(wait_job "$(field "$OVERLAP" result.job_id)" 2>&1 || true)"
[[ "$OVERLAP_JOB" == *'"status":"failed"'* ]] \
    || die "cross-VPC overlap이 실패 Job으로 끝나지 않았다: $OVERLAP_JOB"
HOST_OVERLAP="$(rpc "{\"jsonrpc\":\"2.0\",\"method\":\"vpc.subnet.create\",\"params\":{\"tenant\":\"$TENANT\",\"vpc_id\":\"$VPC_B_ID\",\"name\":\"host-overlap\",\"cidr\":\"192.0.2.0/24\",\"mtu\":1500,\"expected_revision\":$REV_B},\"id\":\"host-overlap\"}")"
HOST_OVERLAP_JOB="$(wait_job "$(field "$HOST_OVERLAP" result.job_id)" 2>&1 || true)"
[[ "$HOST_OVERLAP_JOB" == *'"status":"failed"'* ]] \
    || die "host overlap이 실패 Job으로 끝나지 않았다: $HOST_OVERLAP_JOB"
pass "cross-VPC CIDR와 host connected CIDR 중첩 거부"

create_attachment()
{
    local subnet_id="$1"
    local vm="$2"
    local job result
    job="$(rpc_mutation "{\"jsonrpc\":\"2.0\",\"method\":\"vpc.attachment.create\",\"params\":{\"tenant\":\"$TENANT\",\"subnet_id\":\"$subnet_id\",\"vm\":\"$vm\"},\"id\":\"create-attachment\"}")"
    result="$(job_result "$job")"
    printf '%s\t%s\t%s\n' "$(field "$result" id)" "$(field "$result" ip_address)" \
        "$(field "$result" mac_address)"
}

IFS=$'\t' read -r ATTACHMENT_A_ID ATTACHMENT_A_IP ATTACHMENT_A_MAC \
    < <(create_attachment "$SUBNET_A1_ID" "$VM_A")
IFS=$'\t' read -r ATTACHMENT_B_ID ATTACHMENT_B_IP ATTACHMENT_B_MAC \
    < <(create_attachment "$SUBNET_A2_ID" "$VM_B")

INACTIVE_A="$(virsh dumpxml --inactive "$VM_A")"
[[ "$INACTIVE_A" == *"$ATTACHMENT_A_MAC"* && "$INACTIVE_A" == *"$BRIDGE_A1"* &&
   "$INACTIVE_A" == *"urn:purecvisor:vpc"* && "$INACTIVE_A" == *"$ATTACHMENT_A_ID"* ]] \
    || die "VM A inactive XML/metadata가 attachment DB와 다르다"
grep -Fq "dhcp-host=$ATTACHMENT_A_MAC,$ATTACHMENT_A_IP,infinite" \
    "/run/purecvisor/network/dnsmasq-$BRIDGE_A1.conf" \
    || die "VM A static DHCP lease가 없다"
pass "attachment DB, inactive libvirt NIC/metadata, dnsmasq static lease 일치"

virsh start "$VM_A" >/dev/null
VM_A_RUNNING=1
virsh start "$VM_B" >/dev/null
VM_B_RUNNING=1
wait_guest "$VM_A" "$ATTACHMENT_A_IP" || die "VM A guest shell/DHCP가 준비되지 않았다"
wait_guest "$VM_B" "$ATTACHMENT_B_IP" || die "VM B guest shell/DHCP가 준비되지 않았다"

LIVE_A="$(virsh dumpxml "$VM_A")"
[[ "$LIVE_A" == *"$ATTACHMENT_A_MAC"* && "$LIVE_A" == *"$BRIDGE_A1"* ]] \
    || die "VM A live XML에 VPC NIC가 없다"
guest_run "$VM_A" "ip -4 addr show dev eth0 | grep -q 'inet $ATTACHMENT_A_IP/24'" \
    >/dev/null || die "VM A가 static DHCP 주소를 받지 못했다"
guest_run "$VM_B" "ip -4 addr show dev eth0 | grep -q 'inet $ATTACHMENT_B_IP/24'" \
    >/dev/null || die "VM B가 static DHCP 주소를 받지 못했다"
pass "정지 VM persistent attach 뒤 부팅과 DHCP reservation 일치"

ip netns add "$SOURCE_NS"
SOURCE_NS_CREATED=1
ip link add "$SOURCE_VETH_HOST" type veth peer name "$SOURCE_VETH_NS"
ip link set "$SOURCE_VETH_NS" netns "$SOURCE_NS"
ip addr add "$SOURCE_HOST_IP/29" dev "$SOURCE_VETH_HOST"
ip link set "$SOURCE_VETH_HOST" up
ip -n "$SOURCE_NS" link set lo up
ip -n "$SOURCE_NS" addr add "$SOURCE_NS_IP/29" dev "$SOURCE_VETH_NS"
ip -n "$SOURCE_NS" addr add "$SOURCE_DENIED_IP/29" dev "$SOURCE_VETH_NS"
ip -n "$SOURCE_NS" link set "$SOURCE_VETH_NS" up
ip -n "$SOURCE_NS" route add default via "$SOURCE_HOST_IP"

guest_run "$VM_A" "ping -c 2 -W 1 $ATTACHMENT_B_IP" >/dev/null \
    || die "같은 VPC의 서로 다른 subnet 간 L3 통신이 실패했다"
pass "같은 VPC의 서로 다른 subnet 간 L3 통신 허용"

virsh destroy "$VM_B" >/dev/null
VM_B_RUNNING=0
rpc_mutation "{\"jsonrpc\":\"2.0\",\"method\":\"vpc.attachment.delete\",\"params\":{\"tenant\":\"$TENANT\",\"attachment_id\":\"$ATTACHMENT_B_ID\"},\"id\":\"move-b-delete\"}" >/dev/null
ATTACHMENT_B_ID=""
IFS=$'\t' read -r ATTACHMENT_B_ID ATTACHMENT_B_IP ATTACHMENT_B_MAC \
    < <(create_attachment "$SUBNET_B_ID" "$VM_B")
virsh start "$VM_B" >/dev/null
VM_B_RUNNING=1
wait_guest "$VM_B" "$ATTACHMENT_B_IP" || die "VPC B로 이동한 VM B가 준비되지 않았다"

guest_run "$VM_A" "ping -c 2 -W 1 $SOURCE_NS_IP" >/dev/null \
    || die "NAT VPC outbound/reply가 실패했다"
if guest_run "$VM_A" "ping -c 1 -W 1 $ATTACHMENT_B_IP" >/dev/null; then
    die "cross-VPC 신규 트래픽이 통과했다"
fi
if guest_run "$VM_A" "nc -w 2 192.0.2.53 22 </dev/null" >/dev/null; then
    die "VPC VM의 host SSH 접근이 통과했다"
fi
pass "NAT outbound/reply 허용, cross-VPC와 host input 차단"

if guest_run "$VM_A" \
    "ip addr add 10.253.10.250/24 dev eth0; ping -I 10.253.10.250 -c 2 -W 1 10.253.10.1" \
    >/dev/null; then
    die "spoof source IP가 gateway에 도달했다"
fi
guest_run "$VM_A" "ip addr del 10.253.10.250/24 dev eth0" >/dev/null \
    || die "guest spoof test address cleanup 실패"
pass "관리 MAC/IP pair 외 IPv4 source spoof drop"

SG_CREATE="$(rpc "{\"jsonrpc\":\"2.0\",\"method\":\"security_group.create\",\"params\":{\"name\":\"$SG_NAME\",\"description\":\"Local VPC live gate\"},\"id\":\"sg-create\"}")"
assert_field "$SG_CREATE" result true "Security Group 생성"
SG_CREATED=1
SG_RULE="$(rpc "{\"jsonrpc\":\"2.0\",\"method\":\"security_group.rule.add\",\"params\":{\"name\":\"$SG_NAME\",\"direction\":\"ingress\",\"protocol\":\"tcp\",\"port\":$TARGET_PORT,\"source\":\"$SOURCE_PREFIX\"},\"id\":\"sg-rule\"}")"
assert_field "$SG_RULE" result true "Security Group ingress 규칙"
SG_ATTACH="$(rpc "{\"jsonrpc\":\"2.0\",\"method\":\"vm.security_group.set\",\"params\":{\"vm\":\"$VM_A\",\"security_group\":\"$SG_NAME\"},\"id\":\"sg-attach\"}")"
assert_field "$SG_ATTACH" result true "Security Group attachment"
SG_BOUND=1

PUBLISH_JOB="$(rpc_mutation "{\"jsonrpc\":\"2.0\",\"method\":\"vpc.service.publish\",\"params\":{\"tenant\":\"$TENANT\",\"attachment_id\":\"$ATTACHMENT_A_ID\",\"protocol\":\"tcp\",\"listen_address\":\"$SOURCE_HOST_IP\",\"listen_port\":$PUBLISH_PORT,\"target_port\":$TARGET_PORT,\"allowed_sources\":[\"$SOURCE_NS_IP/32\"]},\"id\":\"publish\"}")"
PUBLISH_RESULT="$(job_result "$PUBLISH_JOB")"
PUBLISH_ID="$(field "$PUBLISH_RESULT" id)"

PUBLISHED_REPLY="$(ip netns exec "$SOURCE_NS" sh -c "printf x | nc -w 3 $SOURCE_HOST_IP $PUBLISH_PORT" 2>/dev/null || true)"
[[ "$PUBLISHED_REPLY" == *"published"* ]] \
    || die "허용 source의 Service Publish가 target에 도달하지 않았다"
if ip netns exec "$SOURCE_NS" nc -s "$SOURCE_DENIED_IP" -w 2 \
    "$SOURCE_HOST_IP" "$PUBLISH_PORT" </dev/null >/dev/null 2>&1; then
    die "미허용 source가 Service Publish에 도달했다"
fi
if ip netns exec "$SOURCE_NS" nc -w 2 "$SOURCE_HOST_IP" "$((PUBLISH_PORT + 1))" \
    </dev/null >/dev/null 2>&1; then
    die "미게시 port가 도달했다"
fi
pass "Service Publish 허용 source 성공, 미허용 source·미게시 port 차단"

virsh destroy "$VM_B" >/dev/null
VM_B_RUNNING=0
rpc_mutation "{\"jsonrpc\":\"2.0\",\"method\":\"vpc.attachment.delete\",\"params\":{\"tenant\":\"$TENANT\",\"attachment_id\":\"$ATTACHMENT_B_ID\"},\"id\":\"isolated-move-delete\"}" >/dev/null
ATTACHMENT_B_ID=""
IFS=$'\t' read -r ATTACHMENT_B_ID ATTACHMENT_B_IP ATTACHMENT_B_MAC \
    < <(create_attachment "$SUBNET_ISOLATED_ID" "$VM_B")
virsh start "$VM_B" >/dev/null
VM_B_RUNNING=1
wait_guest "$VM_B" "$ATTACHMENT_B_IP" || die "isolated VPC의 VM B가 준비되지 않았다"
if guest_run "$VM_B" "ping -c 1 -W 1 $SOURCE_NS_IP" >/dev/null; then
    die "isolated VPC outbound가 통과했다"
fi
SG_ATTACH_B="$(rpc "{\"jsonrpc\":\"2.0\",\"method\":\"vm.security_group.set\",\"params\":{\"vm\":\"$VM_B\",\"security_group\":\"$SG_NAME\"},\"id\":\"sg-attach-b\"}")"
assert_field "$SG_ATTACH_B" result true "isolated VM Security Group attachment"
SG_B_BOUND=1
ISOLATED_PUBLISH="$(rpc "{\"jsonrpc\":\"2.0\",\"method\":\"vpc.service.publish\",\"params\":{\"tenant\":\"$TENANT\",\"attachment_id\":\"$ATTACHMENT_B_ID\",\"protocol\":\"tcp\",\"listen_address\":\"$SOURCE_HOST_IP\",\"listen_port\":$((PUBLISH_PORT + 2)),\"target_port\":$TARGET_PORT,\"allowed_sources\":[\"$SOURCE_NS_IP/32\"]},\"id\":\"isolated-publish\"}")"
ISOLATED_JOB="$(wait_job "$(field "$ISOLATED_PUBLISH" result.job_id)" 2>&1 || true)"
[[ "$ISOLATED_JOB" == *'"status":"failed"'* ]] \
    || die "isolated VPC Service Publish가 실패하지 않았다: $ISOLATED_JOB"
pass "isolated outbound와 Service Publish 거부"

BEFORE_RESTART_VPC_XML="$(vpc_xml_fingerprint \
    "$VM_A" "$ATTACHMENT_A_MAC" "$ATTACHMENT_A_ID")"
systemctl restart "$SERVICE"
wait_ready || die "daemon 재시작 뒤 VPC RPC가 응답하지 않는다"
AFTER_RESTART_STATUS="$(rpc '{"jsonrpc":"2.0","method":"vpc.status","params":{},"id":"after-restart"}')"
assert_field "$AFTER_RESTART_STATUS" result.healthy true "재시작 후 VPC 상태"
assert_field "$AFTER_RESTART_STATUS" result.reconcile_required false "재시작 후 reconcile 상태"
AFTER_RESTART_VPC_XML="$(vpc_xml_fingerprint \
    "$VM_A" "$ATTACHMENT_A_MAC" "$ATTACHMENT_A_ID")"
[[ "$AFTER_RESTART_VPC_XML" == "$BEFORE_RESTART_VPC_XML" ]] \
    || die "재시작 reconcile 뒤 VPC NIC/metadata projection이 바뀌었다: before=$BEFORE_RESTART_VPC_XML after=$AFTER_RESTART_VPC_XML"
pgrep -a -x dnsmasq | grep -Fq "dnsmasq-$BRIDGE_A1.conf" \
    || die "재시작 reconcile 뒤 dnsmasq가 없다"
nft list table inet pcv_vpc | grep -Fq "$ATTACHMENT_A_IP" \
    || die "재시작 reconcile 뒤 Service Publish target rule이 없다"
pass "daemon 재시작 뒤 bridge/DHCP/libvirt/nft desired state 수렴"

AUDIT_OK="$(sqlite3 "$AUDIT_DB" "SELECT count(*) FROM audit_log WHERE id > $AUDIT_BASE_ID AND username='local-admin' AND method LIKE 'vpc.%' AND target LIKE '$TENANT/%' AND result='ok';")"
AUDIT_FAIL="$(sqlite3 "$AUDIT_DB" "SELECT count(*) FROM audit_log WHERE id > $AUDIT_BASE_ID AND username='local-admin' AND method='vpc.subnet.create' AND target LIKE '$TENANT/%' AND result='fail';")"
[[ "$AUDIT_OK" -gt 0 && "$AUDIT_FAIL" -gt 0 ]] \
    || die "VPC worker 성공/실패 audit 증거가 부족하다: ok=$AUDIT_OK fail=$AUDIT_FAIL"
pass "accepted Job의 worker 성공·실패 audit 기록"

log "PASS — Local VPC privileged live gate $PASS_COUNT개 완료; EXIT cleanup에서 자원을 회수한다"
