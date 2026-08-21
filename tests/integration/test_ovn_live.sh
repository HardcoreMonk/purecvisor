#!/usr/bin/env bash
                          
                                                                          
                                                                    
                                                                       
                      
                                                       
 
                                                                     
set -Eeuo pipefail

readonly SOCKET="/var/run/purecvisor/daemon.sock"
readonly PCVCTL="${PCVCTL:-/usr/local/bin/pcvctl}"
readonly LSA="pcv-live-a"
readonly LSB="pcv-live-b"
readonly LR="pcv-live-r"
readonly NSA="pcvovna"
readonly NSB="pcvovnb"
readonly NSC="pcvovnc"
readonly IFA="ovnvpa"
readonly IFB="ovnvpb"
readonly IFC="ovnvpc"
readonly TENANT_A="pcv-live-ta"
readonly TENANT_B="pcv-live-tb"
readonly TENANT_SWA="tenant-pcv-live-ta-ls"
readonly TENANT_SWB="tenant-pcv-live-tb-ls"
readonly TENANT_NSA="pcvovnta"
readonly TENANT_NSB="pcvovntb"
readonly TENANT_IFA="ovnvtpa"
readonly TENANT_IFB="ovnvtpb"
BASE_DEFAULT_ROUTE=""

log() { printf '[OVN-LIVE] %s\n' "$*"; }
die() { log "FAIL — $*" >&2; exit 1; }

rpc()
{
    local method="$1" params="$2" response
    response="$(printf '{"jsonrpc":"2.0","method":"%s","params":%s,"id":1}' \
        "$method" "$params" | socat - UNIX-CONNECT:"$SOCKET")"
    printf '%s\n' "$response"
    [[ "$response" == *'"result"'* ]] || return 1
}

cleanup()
{
    local exit_code=$?
    trap - EXIT INT TERM
    set +e
    for iface in "$IFA" "$IFB" "$IFC" "$TENANT_IFA" "$TENANT_IFB"; do
        ovs-vsctl --if-exists del-port br-int "$iface"
    done
    for namespace in "$NSA" "$NSB" "$NSC" "$TENANT_NSA" "$TENANT_NSB"; do
        ip netns delete "$namespace" 2>/dev/null
    done
    "$PCVCTL" --format=json ovn router delete "$LR" >/dev/null 2>&1
    "$PCVCTL" --format=json ovn switch delete "$LSA" >/dev/null 2>&1
    "$PCVCTL" --format=json ovn switch delete "$LSB" >/dev/null 2>&1
    "$PCVCTL" --format=json ovn switch delete "$TENANT_SWA" >/dev/null 2>&1
    "$PCVCTL" --format=json ovn switch delete "$TENANT_SWB" >/dev/null 2>&1
    for switch in "$LSA" "$LSB" "$TENANT_SWA" "$TENANT_SWB"; do
        ovn-nbctl --data=bare --no-heading --columns=_uuid find DHCP_Options \
            "external_ids:logical_switch=$switch" | while read -r uuid; do
            [[ -n "$uuid" ]] && ovn-nbctl destroy DHCP_Options "$uuid"
        done
    done
    if [[ -n "$BASE_DEFAULT_ROUTE" && "$(ip route show default)" != "$BASE_DEFAULT_ROUTE" ]]; then
        log "FAIL — cleanup 뒤 default route가 기준선과 다르다" >&2
        exit_code=1
    fi
    exit "$exit_code"
}
trap cleanup EXIT INT TERM

[[ "$EUID" -eq 0 ]] || die "root 권한이 필요하다"
[[ "${PCV_OVN_LIVE:-}" == "1" ]] || die "PCV_OVN_LIVE=1 명시 opt-in이 필요하다"
for command in "$PCVCTL" socat ovs-vsctl ovn-nbctl ovn-sbctl ip ping busybox timeout; do
    command -v "$command" >/dev/null || die "필수 명령이 없다: $command"
done
systemctl is-active --quiet purecvisorsd.service || die "purecvisorsd가 active가 아니다"
systemctl is-active --quiet ovn-central.service || die "ovn-central이 active가 아니다"
systemctl is-active --quiet ovn-controller.service || die "ovn-controller가 active가 아니다"
ovs-vsctl br-exists br-int || die "br-int가 없다"
"$PCVCTL" --format=json ovn status | grep -q '"available":true' || die "제품 OVN 상태가 available이 아니다"
for name in "$LSA" "$LSB" "$TENANT_SWA" "$TENANT_SWB"; do
    ovn-nbctl ls-list | grep -Fq "($name)" && die "시험 switch가 이미 존재한다: $name"
done
ovn-nbctl lr-list | grep -Fq "($LR)" && die "시험 router가 이미 존재한다: $LR"
for namespace in "$NSA" "$NSB" "$NSC" "$TENANT_NSA" "$TENANT_NSB"; do
    ip netns list | awk '{print $1}' | grep -Fxq "$namespace" &&
        die "시험 namespace가 이미 존재한다: $namespace"
done
BASE_DEFAULT_ROUTE="$(ip route show default)"
log "PASS 1 — service, DB, controller, br-int와 이름 충돌 preflight"

"$PCVCTL" --format=json ovn switch create "$LSA" --subnet 10.252.10.0/24
"$PCVCTL" --format=json ovn switch create "$LSB" --subnet 10.252.20.0/24
"$PCVCTL" --format=json ovn router create "$LR"
"$PCVCTL" --format=json ovn dhcp enable 10.252.10.0/24 10.252.10.1 --switch "$LSA"
"$PCVCTL" --format=json ovn dhcp enable 10.252.20.0/24 10.252.20.1 --switch "$LSB"
rpc ovn.port.add '{"switch":"pcv-live-a","port":"pcv-live-p1","mac":"02:00:00:00:10:11","ip":"10.252.10.11"}'
rpc ovn.port.add '{"switch":"pcv-live-a","port":"pcv-live-p2","mac":"02:00:00:00:10:12","ip":"10.252.10.12"}'
rpc ovn.port.add '{"switch":"pcv-live-b","port":"pcv-live-p3","mac":"02:00:00:00:20:21","ip":"10.252.20.21"}'
rpc ovn.router.add_port '{"router":"pcv-live-r","switch":"pcv-live-a","mac":"02:00:00:00:10:01","cidr":"10.252.10.1/24"}'
rpc ovn.router.add_port '{"router":"pcv-live-r","switch":"pcv-live-b","mac":"02:00:00:00:20:01","cidr":"10.252.20.1/24"}'
rpc ovn.nat.add '{"router":"pcv-live-r","type":"snat","external_ip":"192.0.2.10","logical_ip":"10.252.10.0/24"}'
log "PASS 2 — 제품 RPC LS/LSP/DHCP/LR/NAT 생성"

ovs-vsctl --may-exist add-port br-int "$IFA" -- set Interface "$IFA" type=internal external_ids:iface-id=pcv-live-p1
ovs-vsctl --may-exist add-port br-int "$IFB" -- set Interface "$IFB" type=internal external_ids:iface-id=pcv-live-p2
ovs-vsctl --may-exist add-port br-int "$IFC" -- set Interface "$IFC" type=internal external_ids:iface-id=pcv-live-p3
for namespace in "$NSA" "$NSB" "$NSC"; do ip netns add "$namespace"; done
ip link set "$IFA" netns "$NSA"
ip link set "$IFB" netns "$NSB"
ip link set "$IFC" netns "$NSC"
for namespace in "$NSA" "$NSB" "$NSC"; do ip -n "$namespace" link set lo up; done
ip -n "$NSA" link set "$IFA" address 02:00:00:00:10:11 up
ip -n "$NSB" link set "$IFB" address 02:00:00:00:10:12 up
ip -n "$NSC" link set "$IFC" address 02:00:00:00:20:21 up
ovn-nbctl --timeout=10 --wait=hv sync

DHCP_OUT="$(timeout 10 ip netns exec "$NSB" busybox udhcpc -f -n -q -t 3 -T 1 \
    -i "$IFB" -s /bin/true 2>&1)"
printf '%s\n' "$DHCP_OUT"
[[ "$DHCP_OUT" == *'lease of 10.252.10.12 obtained'* ]] || die "OVN DHCP lease가 다르다"
log "PASS 3 — 분산 DHCP가 LSP 고정 주소와 gateway를 실제 제공"

ip -n "$NSA" address add 10.252.10.11/24 dev "$IFA"
ip -n "$NSB" address add 10.252.10.12/24 dev "$IFB"
ip -n "$NSC" address add 10.252.20.21/24 dev "$IFC"
ip -n "$NSA" route add 10.252.20.0/24 via 10.252.10.1
ip -n "$NSC" route add 10.252.10.0/24 via 10.252.20.1
ip netns exec "$NSA" ping -c 3 -W 2 10.252.10.12
ip netns exec "$NSA" ping -c 3 -W 2 10.252.20.21
log "PASS 4 — 같은 LS L2와 logical router 간 L3 실제 통신"

"$PCVCTL" --format=json ovn acl add "$LSA" to-lport 2000 \
    'outport == "pcv-live-p2" && ip4 && icmp4' drop
ovn-nbctl --timeout=10 --wait=hv sync
if ip netns exec "$NSA" ping -c 2 -W 1 10.252.10.12 >/dev/null 2>&1; then
    die "OVN ACL drop 뒤 ICMP가 통과했다"
fi
ovn-nbctl lsp-get-dhcpv4-options pcv-live-p2 | grep -Fq '(10.252.10.0/24)' ||
    die "LSP에 DHCP_Options가 연결되지 않았다"
ovn-nbctl lsp-get-port-security pcv-live-p2 | grep -Fq '02:00:00:00:10:12 10.252.10.12' ||
    die "LSP에 MAC/IP port-security가 연결되지 않았다"
ovn-nbctl lr-nat-list "$LR" | grep -Fq '10.252.10.0/24' || die "SNAT rule이 없다"
log "PASS 5 — ACL packet drop, LSP DHCP·port-security 연결, SNAT control state"

rpc ovn.tenant.create '{"tenant":"pcv-live-ta","subnet":"10.252.30.0/24"}'
rpc ovn.tenant.create '{"tenant":"pcv-live-tb","subnet":"10.252.30.0/24"}'
rpc ovn.port.add '{"switch":"tenant-pcv-live-ta-ls","port":"pcv-live-tpa","mac":"02:00:00:00:30:11","ip":"10.252.30.11"}'
rpc ovn.port.add '{"switch":"tenant-pcv-live-tb-ls","port":"pcv-live-tpb","mac":"02:00:00:00:30:12","ip":"10.252.30.12"}'
for switch in "$TENANT_SWA" "$TENANT_SWB"; do
    [[ "$(ovn-nbctl acl-list "$switch" | grep -Ec 'to-lport|from-lport')" -eq 2 ]] \
        || die "$switch의 양방향 tenant ACL이 완전하지 않다"
    ovn-nbctl --data=bare --no-heading --columns=_uuid find DHCP_Options \
        "external_ids:logical_switch=$switch" | grep -Eq '.+' \
        || die "$switch의 DHCP_Options가 없다"
done
ovs-vsctl --may-exist add-port br-int "$TENANT_IFA" -- set Interface "$TENANT_IFA" type=internal external_ids:iface-id=pcv-live-tpa
ovs-vsctl --may-exist add-port br-int "$TENANT_IFB" -- set Interface "$TENANT_IFB" type=internal external_ids:iface-id=pcv-live-tpb
ip netns add "$TENANT_NSA"
ip netns add "$TENANT_NSB"
ip link set "$TENANT_IFA" netns "$TENANT_NSA"
ip link set "$TENANT_IFB" netns "$TENANT_NSB"
ip -n "$TENANT_NSA" link set lo up
ip -n "$TENANT_NSB" link set lo up
ip -n "$TENANT_NSA" link set "$TENANT_IFA" address 02:00:00:00:30:11 up
ip -n "$TENANT_NSB" link set "$TENANT_IFB" address 02:00:00:00:30:12 up
ip -n "$TENANT_NSA" address add 10.252.30.11/24 dev "$TENANT_IFA"
ip -n "$TENANT_NSB" address add 10.252.30.12/24 dev "$TENANT_IFB"
ovn-nbctl --timeout=10 --wait=hv sync
if ip netns exec "$TENANT_NSA" ping -c 2 -W 1 10.252.30.12 >/dev/null 2>&1; then
    die "독립 tenant logical switch 사이 트래픽이 통과했다"
fi
log "PASS 6 — tenant bundle LS·양방향 ACL·DHCP 구축과 동일 CIDR tenant L2 격리"

log "PASS — OVN privileged live gate 완료; EXIT cleanup 실행"
