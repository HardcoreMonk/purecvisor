#!/usr/bin/env bash
                          
                                                                  
                                                                       
                                                                   
                      
                                                      
 
     
                                                                           

set -Eeuo pipefail

readonly SERVICE="purecvisorsd.service"
readonly PCVCTL="${PCVCTL:-/usr/local/bin/pcvctl}"
readonly VM="pcv-nic-cli-live"
readonly BRIDGE="${PCV_NIC_LIVE_BRIDGE:-pcvbr0}"
readonly GATEWAY="${PCV_NIC_LIVE_GATEWAY:-192.0.2.1}"
readonly HOST_IP="${PCV_NIC_LIVE_HOST_IP:-192.0.2.53}"
readonly EXCLUDED_ADDRESS="192.0.2.100"

TMPDIR_LIVE=""
INITRAMFS=""
KERNEL_COPY=""
VM_DEFINED=0
VM_RUNNING=0
BASE_DEFAULT_ROUTE=""
HOTPLUG_MAC=""

log() { printf '[NIC-LIVE] %s\n' "$*"; }
die() { log "FAIL — $*" >&2; exit 1; }

cleanup()
{
    local exit_code=$?
    trap - EXIT INT TERM
    set +e
    if [[ "$VM_RUNNING" -eq 1 ]] && virsh domstate "$VM" >/dev/null 2>&1; then
        virsh destroy "$VM" >/dev/null 2>&1
    fi
    if [[ "$VM_DEFINED" -eq 1 ]] && virsh dominfo "$VM" >/dev/null 2>&1; then
        virsh undefine "$VM" --nvram >/dev/null 2>&1 || virsh undefine "$VM" >/dev/null 2>&1
    fi
    if [[ -n "$TMPDIR_LIVE" && -d "$TMPDIR_LIVE" ]]; then
        rm -rf -- "$TMPDIR_LIVE"
    fi
    if [[ -n "$BASE_DEFAULT_ROUTE" && "$(ip route show default)" != "$BASE_DEFAULT_ROUTE" ]]; then
        log "FAIL — cleanup 뒤 default route가 기준선과 다르다" >&2
        exit_code=1
    fi
    exit "$exit_code"
}
trap cleanup EXIT INT TERM

[[ "$EUID" -eq 0 ]] || die "root 권한이 필요하다"
[[ "${PCV_NIC_LIVE:-}" == "1" ]] || die "PCV_NIC_LIVE=1 명시 opt-in이 필요하다"
for command in "$PCVCTL" systemctl virsh ip bridge cpio gzip busybox file jq; do
    command -v "$command" >/dev/null || die "필수 명령이 없다: $command"
done
[[ "$(file -b /usr/bin/busybox)" == *"statically linked"* ]] \
    || die "/usr/bin/busybox가 정적 실행 파일이 아니다"
if ip -o -4 addr show | awk '{print $4}' | grep -qE "^${EXCLUDED_ADDRESS//./\\.}/"; then
    die "제외 노드 $EXCLUDED_ADDRESS 에서는 실행할 수 없다"
fi
systemctl is-active --quiet "$SERVICE" || die "$SERVICE가 active가 아니다"
ip link show "$BRIDGE" >/dev/null 2>&1 || die "bridge가 없다: $BRIDGE"
ip -o -4 address show | grep -Fq "$HOST_IP/" \
    || die "host에 예상 관리 주소 $HOST_IP가 없다"
ip route get "$GATEWAY" >/dev/null || die "$GATEWAY host 경로가 없다"
"$PCVCTL" --format=json network list | jq -e --arg bridge "$BRIDGE" '
    .result[] | select(.name == $bridge) |
    .mode == "bridge" and .uplink_mode == "shared" and
    .dataplane == "tc-bpf-portal" and .host_l3_preserved == true
' >/dev/null || die "$BRIDGE가 host L3 보존 shared physical bridge가 아니다"
virsh dominfo "$VM" >/dev/null 2>&1 && die "임시 VM이 이미 존재한다: $VM"
BASE_DEFAULT_ROUTE="$(ip route show default)"
log "PASS 1 — service, shared bridge, host route와 이름 충돌 preflight"

mkdir -p /var/lib/libvirt/boot
TMPDIR_LIVE="$(mktemp -d /var/lib/libvirt/boot/pcv-nic-live.XXXXXX)"
chmod 0755 "$TMPDIR_LIVE"
mkdir -p "$TMPDIR_LIVE/rootfs"/{bin,dev,etc/udhcpc,proc,sys,run,tmp}
cp /usr/bin/busybox "$TMPDIR_LIVE/rootfs/bin/busybox"
for applet in sh mount ip udhcpc ping sleep poweroff; do
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

cat >"$TMPDIR_LIVE/rootfs/init" <<EOF
#!/bin/sh
/bin/mount -t proc proc /proc
/bin/mount -t sysfs sysfs /sys
/bin/mount -t devtmpfs devtmpfs /dev
/bin/ip link set lo up
while [ ! -e /sys/class/net/eth0 ]; do /bin/sleep 1; done
/bin/ip link set eth0 up
/bin/udhcpc -i eth0 -n -q -t 20 -T 1
/bin/ping -c 3 -W 2 $GATEWAY
/bin/ping -c 3 -W 2 $HOST_IP
echo "pcv-nic-live guest ready"
exec /bin/sh
EOF
chmod 0755 "$TMPDIR_LIVE/rootfs/init" "$TMPDIR_LIVE/rootfs/etc/udhcpc/default.script"
INITRAMFS="$TMPDIR_LIVE/pcv-nic-live-initramfs.gz"
(cd "$TMPDIR_LIVE/rootfs" && find . -print0 | cpio --null -ov --format=newc 2>/dev/null) \
    | gzip -9 >"$INITRAMFS"
KERNEL_COPY="$TMPDIR_LIVE/vmlinuz"
cp "/boot/vmlinuz-$(uname -r)" "$KERNEL_COPY"
chmod 0644 "$INITRAMFS" "$KERNEL_COPY"

cat >"$TMPDIR_LIVE/$VM.xml" <<EOF
<domain type='kvm'>
  <name>$VM</name>
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
    <serial type='file'><source path='$TMPDIR_LIVE/console.log'/><target port='0'/></serial>
    <console type='file'><source path='$TMPDIR_LIVE/console.log'/><target type='serial' port='0'/></console>
    <memballoon model='none'/>
  </devices>
</domain>
EOF
virsh define "$TMPDIR_LIVE/$VM.xml" >/dev/null
VM_DEFINED=1
virsh start "$VM" >/dev/null
VM_RUNNING=1
log "PASS 2 — 디스크 없는 임시 KVM 기동"

"$PCVCTL" --format=json nic add "$VM" "$BRIDGE" | jq -e '.result == true' >/dev/null

for _attempt in $(seq 1 60); do
    NIC_LIST="$("$PCVCTL" --format=json nic list "$VM")"
    HOTPLUG_MAC="$(printf '%s' "$NIC_LIST" | jq -r --arg bridge "$BRIDGE" \
        '.result[] | select(.bridge == $bridge) | .mac' | head -n 1)"
    GUEST_IP="$(printf '%s' "$NIC_LIST" | jq -r --arg bridge "$BRIDGE" \
        '.result[] | select(.bridge == $bridge) | .ip' | head -n 1)"
    [[ -n "$HOTPLUG_MAC" && -n "$GUEST_IP" ]] && break
    sleep 1
done
[[ -n "$HOTPLUG_MAC" ]] || die "nic list에 $BRIDGE NIC이 없다"
[[ -n "${GUEST_IP:-}" ]] || die "upstream DHCP/neighbor로 guest IP를 확인하지 못했다"
virsh dumpxml "$VM" | grep -Fiq "address='$HOTPLUG_MAC'" || die "live XML에 NIC MAC이 없다"
virsh dumpxml --inactive "$VM" | grep -Fiq "address='$HOTPLUG_MAC'" || die "config XML에 NIC MAC이 없다"
virsh dumpxml --inactive "$VM" | grep -Fq "bridge='$BRIDGE'" || die "config XML bridge source가 다르다"
virsh dumpxml --inactive "$VM" | grep -Fq "filter='no-mac-spoofing'" \
    || die "shared bridge MAC spoof filter가 없다"
ping -c 3 -W 2 "$GUEST_IP" >/dev/null || die "host→guest $GUEST_IP 통신 실패"
grep -Fq "pcv-nic-live guest ready" "$TMPDIR_LIVE/console.log" \
    || die "guest가 DHCP, gateway, host ping을 끝내지 못했다"
log "PASS 3 — nic add/list, upstream DHCP $GUEST_IP, gateway·host 양방향 통신"

"$PCVCTL" --format=json nic remove "$VM" "$HOTPLUG_MAC" | jq -e '.result == true' >/dev/null
for _attempt in $(seq 1 30); do
    if ! virsh dumpxml "$VM" | grep -Fiq "address='$HOTPLUG_MAC'" &&
       ! virsh dumpxml --inactive "$VM" | grep -Fiq "address='$HOTPLUG_MAC'"; then
        break
    fi
    sleep 0.2
done
virsh dumpxml "$VM" | grep -Fiq "address='$HOTPLUG_MAC'" && die "live XML에 제거한 NIC이 남았다"
virsh dumpxml --inactive "$VM" | grep -Fiq "address='$HOTPLUG_MAC'" && die "config XML에 제거한 NIC이 남았다"
"$PCVCTL" --format=json nic list "$VM" | jq -e --arg mac "$HOTPLUG_MAC" \
    '.result | all((.mac | ascii_downcase) != ($mac | ascii_downcase))' >/dev/null
log "PASS 4 — nic remove가 완전한 interface XML로 live/config 모두 제거"

log "PASS — 실제 KVM bridge→NIC→DHCP→통신→detach gate 완료; EXIT cleanup 실행"
