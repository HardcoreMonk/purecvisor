#!/usr/bin/env bash
                          
                                                                      
                                                      
                                                             
                                                              
                                                    
 
                      
                                                     
                                                                    
 
                                                                     
set -euo pipefail

verify_only=0
encap_ip=""

usage() {
    printf 'usage: %s [--verify-only] [--encap-ip IPv4]\n' "$0"
}

fail() {
    printf 'OVN Single Edge: FAIL — %s\n' "$1" >&2
    exit 1
}

while (( $# > 0 )); do
    case "$1" in
        --verify-only)
            verify_only=1
            shift
            ;;
        --encap-ip)
            (( $# >= 2 )) || { usage >&2; exit 2; }
            encap_ip="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            usage >&2
            exit 2
            ;;
    esac
done

require_binary() {
    command -v "$1" >/dev/null 2>&1 || fail "required command missing: $1"
}

strip_ovs_value() {
    local value="$1"
    value="${value#\"}"
    value="${value%\"}"
    printf '%s' "$value"
}

verify_ovn() {
    local system_id remote chassis
    for binary in ovs-vsctl ovn-nbctl ovn-sbctl ovn-appctl; do
        require_binary "$binary"
    done
    systemctl is-active --quiet openvswitch-switch.service ||
        fail "openvswitch-switch is not active"
    systemctl is-active --quiet ovn-central.service ||
        fail "ovn-central is not active"
    systemctl is-active --quiet ovn-controller.service ||
        fail "ovn-controller is not active"
    ovn-nbctl --timeout=5 list NB_Global >/dev/null ||
        fail "Northbound DB query failed"
    ovn-sbctl --timeout=5 list SB_Global >/dev/null ||
        fail "Southbound DB query failed"
    ovn-nbctl --timeout=5 --wait=sb sync >/dev/null ||
        fail "ovn-northd did not synchronize NB to SB"

    system_id="$(strip_ovs_value "$(ovs-vsctl --if-exists get Open_vSwitch . external_ids:system-id)")"
    remote="$(strip_ovs_value "$(ovs-vsctl --if-exists get Open_vSwitch . external_ids:ovn-remote)")"
    [[ -n "$system_id" && "$system_id" != '[]' ]] || fail "OVS system-id is missing"
    [[ "$remote" == 'unix:/var/run/ovn/ovnsb_db.sock' ]] ||
        fail "OVS ovn-remote is not the local Southbound DB socket"
    chassis="$(ovn-sbctl --timeout=5 --data=bare --no-heading --columns=name \
        find Chassis "name=$system_id")"
    [[ "$chassis" == "$system_id" ]] || fail "local Chassis is not registered"
    printf 'OVN Single Edge: PASS — NB/SB/northd/controller/chassis ready (system-id=%s)\n' \
        "$system_id"
}

if (( verify_only == 1 )); then
    verify_ovn
    exit 0
fi

(( EUID == 0 )) || fail "installation must run as root"

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y openvswitch-switch ovn-central ovn-host

systemctl enable --now openvswitch-switch.service
systemctl enable --now ovn-central.service

if [[ -z "$encap_ip" ]]; then
    encap_ip="$(ip -4 route get 1.1.1.1 2>/dev/null | awk '{for (i=1;i<=NF;i++) if ($i=="src") {print $(i+1); exit}}')"
fi
[[ "$encap_ip" =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}$ ]] ||
    fail "a canonical local IPv4 encap address is required"
ip -4 address show | grep -q "inet ${encap_ip}/" ||
    fail "encap IPv4 is not assigned to this host"

system_id="$(strip_ovs_value "$(ovs-vsctl --if-exists get Open_vSwitch . external_ids:system-id)")"
if [[ -z "$system_id" || "$system_id" == '[]' ]]; then
    system_id="$(hostname)"
fi
[[ "$system_id" =~ ^[A-Za-z0-9][A-Za-z0-9_.:-]*$ ]] ||
    fail "hostname/system-id contains unsupported characters"

ovs-vsctl set Open_vSwitch . \
    "external_ids:ovn-remote=unix:/var/run/ovn/ovnsb_db.sock" \
    "external_ids:system-id=$system_id" \
    "external_ids:ovn-encap-type=geneve" \
    "external_ids:ovn-encap-ip=$encap_ip"

if systemctl list-unit-files ovn-host.service --no-legend 2>/dev/null | grep -q '^ovn-host.service'; then
    systemctl enable --now ovn-host.service
else
    systemctl enable --now ovn-controller.service
fi
systemctl restart ovn-controller.service

for _ in $(seq 1 30); do
    if verify_ovn >/dev/null 2>&1; then
        verify_ovn
        exit 0
    fi
    sleep 1
done
verify_ovn
