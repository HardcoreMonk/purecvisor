#!/usr/bin/env bash
                                                                        
                                                                  
                                                                            
                                                                          
                                                                  
                                                     

set -euo pipefail

if [[ "${PCV_ALLOW_LIVE_NETWORK_MUTATION:-}" != "1" ]]; then
    echo "SKIP: set PCV_ALLOW_LIVE_NETWORK_MUTATION=1 for the local live gate" >&2
    exit 77
fi
if [[ -z "${PCV_LIVE_NODE_IPV4:-}" ]]; then
    echo "FAIL: PCV_LIVE_NODE_IPV4 must pin the local management IPv4" >&2
    exit 2
fi
if [[ "${PCV_LIVE_NODE_IPV4}" == "192.0.2.100" ]]; then
    echo "FAIL: 192.0.2.100 is explicitly excluded" >&2
    exit 2
fi

pcv_live_br="${PCV_LIVE_BRIDGE:-pcvr7smk}"
pcv_live_cidr="${PCV_LIVE_CIDR:-10.254.253.1/24}"
pcv_live_subnet="${PCV_LIVE_SUBNET:-10.254.253.0/24}"
pcv_live_cli="${PCV_LIVE_CLI:-/usr/local/bin/pcvctl}"
pcv_live_sock="${PCV_LIVE_SOCKET:-/var/run/purecvisor/daemon.sock}"
pcv_live_netdir="${PCV_LIVE_NETWORK_RUNDIR:-/var/run/purecvisor/network}"
pcv_live_service="${PCV_LIVE_SERVICE:-purecvisorsd}"

for pcv_live_cmd in jq ip nft nc systemctl; do
    command -v "${pcv_live_cmd}" >/dev/null || {
        echo "FAIL: missing command: ${pcv_live_cmd}" >&2
        exit 1
    }
done
[[ -x "${pcv_live_cli}" ]] || { echo "FAIL: CLI not executable: ${pcv_live_cli}" >&2; exit 1; }
sudo -n true
systemctl is-active --quiet "${pcv_live_service}"
hostname -I | tr ' ' '\n' | grep -Fxq "${PCV_LIVE_NODE_IPV4}" || {
    echo "FAIL: pinned management IPv4 is not local" >&2
    exit 2
}
if hostname -I | tr ' ' '\n' | grep -Fxq '192.0.2.100'; then
    echo "FAIL: excluded node/address detected" >&2
    exit 2
fi
[[ ! -e "/sys/class/net/${pcv_live_br}" ]] || {
    echo "FAIL: test bridge already exists: ${pcv_live_br}" >&2
    exit 2
}
if ip route show table all | grep -Fq "${pcv_live_subnet}"; then
    echo "FAIL: test subnet already has a route: ${pcv_live_subnet}" >&2
    exit 2
fi
if sudo -n find "${pcv_live_netdir}" -maxdepth 1 -name "dnsmasq-${pcv_live_br}.*" -print -quit | grep -q .; then
    echo "FAIL: stale test runtime files exist" >&2
    exit 2
fi

pcv_live_mgmt_iface=$(ip -4 -o addr show | awk -v pcv_ip="${PCV_LIVE_NODE_IPV4}" \
    '$4 ~ ("^" pcv_ip "/") {print $2; exit}')
[[ -n "${pcv_live_mgmt_iface}" ]] || { echo "FAIL: management interface not resolved" >&2; exit 1; }
pcv_live_addr_before=$(ip -4 -o addr show dev "${pcv_live_mgmt_iface}")
pcv_live_defaults_before=$(ip -4 route show default)
pcv_live_pid_before=$(systemctl show "${pcv_live_service}" -p MainPID --value)
pcv_live_restarts_before=$(systemctl show "${pcv_live_service}" -p NRestarts --value)

pcv_live_rpc() {
    printf '%s\n' "$1" | sudo -n nc -U -w 15 "${pcv_live_sock}"
}

pcv_live_cleanup() {
    pcv_live_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"network.delete\",\"params\":{\"bridge_name\":\"${pcv_live_br}\"},\"id\":\"cleanup\"}" \
        >/dev/null 2>&1 || true
    for _pcv_live_i in $(seq 1 30); do
        [[ ! -e "/sys/class/net/${pcv_live_br}" ]] && return
        sleep 0.2
    done
    echo "WARN: RPC cleanup did not remove ${pcv_live_br}" >&2
}
trap pcv_live_cleanup EXIT INT TERM

pcv_live_cli_json() {
    sudo -n "${pcv_live_cli}" --socket="${pcv_live_sock}" --format=json --no-color "$@"
}

pcv_live_assert_mode() {
    local pcv_live_expected=$1
    pcv_live_cli_json network list | jq -e \
        --arg pcv_br "${pcv_live_br}" --arg pcv_mode "${pcv_live_expected}" \
        '.result[] | select(.name==$pcv_br and .mode==$pcv_mode)' >/dev/null
    sudo -n jq -e --arg pcv_mode "${pcv_live_expected}" \
        '.mode==$pcv_mode' "${pcv_live_netdir}/dnsmasq-${pcv_live_br}.meta" >/dev/null
}

pcv_live_assert_dhcp_alive() {
    local pcv_live_pid_file="${pcv_live_netdir}/dnsmasq-${pcv_live_br}.pid"
    sudo -n test -e "${pcv_live_pid_file}"
    local pcv_live_dns_pid
    pcv_live_dns_pid=$(sudo -n cat "${pcv_live_pid_file}")
    sudo -n kill -0 "${pcv_live_dns_pid}"
    sudo -n test "$(sudo -n cat "/proc/${pcv_live_dns_pid}/comm")" = "dnsmasq"
}

pcv_live_cli_json network create "${pcv_live_br}" \
    --mode nat --cidr "${pcv_live_cidr}" --mtu 9000 \
    | jq -e '.result.status=="created"' >/dev/null
[[ "$(cat "/sys/class/net/${pcv_live_br}/mtu")" == "9000" ]]
ip -4 -o addr show dev "${pcv_live_br}" | grep -Fq "${pcv_live_cidr}"
pcv_live_assert_mode nat
pcv_live_assert_dhcp_alive
sudo -n nft -a list table inet purecvisor | grep -F "\"${pcv_live_br}\"" | grep -q masquerade
echo "PASS create: nat + MTU 9000 + CIDR + DHCP + metadata + masquerade"

pcv_live_cli_json network mode "${pcv_live_br}" isolated "${pcv_live_cidr}" \
    | jq -e '.result.mode=="isolated"' >/dev/null
pcv_live_assert_mode isolated
pcv_live_assert_dhcp_alive
pcv_live_rules=$(sudo -n nft -a list table inet purecvisor | grep -F "\"${pcv_live_br}\"")
grep -q drop <<<"${pcv_live_rules}"
! grep -q masquerade <<<"${pcv_live_rules}"
echo "PASS mode isolated: DHCP converged + drop rules + no masquerade"

pcv_live_cli_json network mode "${pcv_live_br}" routed "${pcv_live_cidr}" \
    | jq -e '.result.mode=="routed"' >/dev/null
pcv_live_assert_mode routed
sudo -n test ! -e "${pcv_live_netdir}/dnsmasq-${pcv_live_br}.pid"
pcv_live_rules=$(sudo -n nft -a list table inet purecvisor | grep -F "\"${pcv_live_br}\"")
! grep -Eq 'masquerade| drop' <<<"${pcv_live_rules}"
echo "PASS mode routed: DHCP stopped + no stale PID + no masquerade/drop"

                                                               
pcv_live_cli_json network mode "${pcv_live_br}" nat "${pcv_live_cidr}" \
    | jq -e '.result.mode=="nat"' >/dev/null
pcv_live_assert_mode nat
pcv_live_assert_dhcp_alive
sudo -n nft -a list table inet purecvisor | grep -F "\"${pcv_live_br}\"" | grep -q masquerade
echo "PASS mode nat: immediate DHCP restart + masquerade"

pcv_live_cli_json network delete "${pcv_live_br}" | jq -e '.result.status=="deleted"' >/dev/null
for _pcv_live_i in $(seq 1 30); do
    [[ ! -e "/sys/class/net/${pcv_live_br}" ]] && break
    sleep 0.2
done
[[ ! -e "/sys/class/net/${pcv_live_br}" ]]
! sudo -n find "${pcv_live_netdir}" -maxdepth 1 -name "dnsmasq-${pcv_live_br}.*" -print -quit | grep -q .
! sudo -n nft -a list table inet purecvisor | grep -Fq "\"${pcv_live_br}\""
[[ "$(ip -4 -o addr show dev "${pcv_live_mgmt_iface}")" == "${pcv_live_addr_before}" ]]
[[ "$(ip -4 route show default)" == "${pcv_live_defaults_before}" ]]
[[ "$(systemctl show "${pcv_live_service}" -p MainPID --value)" == "${pcv_live_pid_before}" ]]
[[ "$(systemctl show "${pcv_live_service}" -p NRestarts --value)" == "${pcv_live_restarts_before}" ]]

trap - EXIT INT TERM
echo "PASS cleanup: no bridge/files/nft residue; management network and daemon unchanged"
