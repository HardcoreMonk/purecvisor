#!/usr/bin/env bash










set -Eeuo pipefail

readonly SOCKET="/var/run/purecvisor/daemon.sock"
readonly RUN_SUFFIX="${PCV_OVN_CLEANUP_RUN_ID:-$$}"
readonly SWITCH_A="pcv-rec-clean-a-${RUN_SUFFIX}"
readonly SWITCH_B="pcv-rec-clean-b-${RUN_SUFFIX}"
EMERGENCY_DESTROY_COUNT=0

log() { printf '[OVN-DHCP-CLEANUP] %s\n' "$*"; }
die() { log "FAIL — $*" >&2; exit 1; }

rpc()
{
    local method="$1" params="$2" response
    response="$(jq -cn --arg method "$method" --argjson params "$params" \
        '{jsonrpc:"2.0",method:$method,params:$params,id:1}' | \
        socat - UNIX-CONNECT:"$SOCKET")"
    printf '%s\n' "$response"
    jq -e '.result != null and .error == null' >/dev/null <<<"$response"
}

dhcp_uuids()
{
    local switch_name="$1"
    ovn-nbctl --data=bare --no-heading --columns=_uuid find DHCP_Options \
        "external_ids:logical_switch=${switch_name}" | sed '/^[[:space:]]*$/d'
}

cleanup()
{
    local exit_code=$?
    trap - EXIT INT TERM
    set +e
    for switch_name in "$SWITCH_A" "$SWITCH_B"; do
        rpc ovn.switch.delete "$(jq -cn --arg name "$switch_name" '{name:$name}')" \
            >/dev/null 2>&1
        while IFS= read -r uuid; do
            [[ -n "$uuid" ]] || continue
            ovn-nbctl --if-exists destroy DHCP_Options "$uuid" >/dev/null 2>&1
            EMERGENCY_DESTROY_COUNT=$((EMERGENCY_DESTROY_COUNT + 1))
        done < <(dhcp_uuids "$switch_name")
    done
    if [[ "$exit_code" -eq 0 && "$EMERGENCY_DESTROY_COUNT" -ne 0 ]]; then
        log "FAIL — 정상 경로에서 emergency DHCP destroy ${EMERGENCY_DESTROY_COUNT}건 실행" >&2
        exit_code=1
    elif [[ "$EMERGENCY_DESTROY_COUNT" -ne 0 ]]; then
        log "cleanup — 실패 후 exact DHCP UUID ${EMERGENCY_DESTROY_COUNT}건 emergency 제거"
    fi
    exit "$exit_code"
}
trap cleanup EXIT INT TERM

[[ "$EUID" -eq 0 ]] || die "root 권한이 필요하다"
[[ "${PCV_OVN_DHCP_CLEANUP_LIVE:-}" == "1" ]] || \
    die "PCV_OVN_DHCP_CLEANUP_LIVE=1 명시 opt-in이 필요하다"
for command_name in jq socat ovn-nbctl systemctl; do
    command -v "$command_name" >/dev/null || die "필수 명령이 없다: $command_name"
done
[[ -S "$SOCKET" ]] || die "제품 UDS가 없다: $SOCKET"
systemctl is-active --quiet purecvisorsd.service || die "purecvisorsd가 active가 아니다"
systemctl is-active --quiet ovn-central.service || die "ovn-central이 active가 아니다"

for switch_name in "$SWITCH_A" "$SWITCH_B"; do
    ovn-nbctl --if-exists get Logical_Switch "$switch_name" name 2>/dev/null | grep -q . &&
        die "시험 switch가 이미 존재한다: $switch_name"
    [[ -z "$(dhcp_uuids "$switch_name")" ]] || die "시험 DHCP marker가 이미 존재한다: $switch_name"
done
log "PASS 1 — service, NB DB와 run-unique 이름 preflight"

rpc ovn.switch.create "$(jq -cn --arg name "$SWITCH_A" '{name:$name}')"
rpc ovn.switch.create "$(jq -cn --arg name "$SWITCH_B" '{name:$name}')"
rpc ovn.dhcp.enable "$(jq -cn --arg switch "$SWITCH_A" \
    '{switch:$switch,subnet:"10.253.41.0/24",gateway:"10.253.41.1"}')"
rpc ovn.dhcp.enable "$(jq -cn --arg switch "$SWITCH_B" \
    '{switch:$switch,subnet:"10.253.42.0/24",gateway:"10.253.42.1"}')"

UUID_A="$(dhcp_uuids "$SWITCH_A")"
UUID_B="$(dhcp_uuids "$SWITCH_B")"
[[ "$(wc -w <<<"$UUID_A")" -eq 1 ]] || die "switch A DHCP UUID가 정확히 1개가 아니다"
[[ "$(wc -w <<<"$UUID_B")" -eq 1 ]] || die "switch B DHCP UUID가 정확히 1개가 아니다"
[[ "$UUID_A" != "$UUID_B" ]] || die "두 switch가 같은 DHCP UUID를 공유한다"
log "PASS 2 — 제품 경로로 독립 switch-owned DHCP 두 건 생성"

rpc ovn.switch.delete "$(jq -cn --arg name "$SWITCH_A" '{name:$name}')"
[[ -z "$(dhcp_uuids "$SWITCH_A")" ]] || die "switch A 삭제 뒤 DHCP orphan이 남았다"
[[ "$(dhcp_uuids "$SWITCH_B")" == "$UUID_B" ]] || \
    die "switch A 삭제가 switch B DHCP를 변경했다"
log "PASS 3 — A cleanup residue 0, B 보존 음성 대조"

rpc ovn.switch.delete "$(jq -cn --arg name "$SWITCH_B" '{name:$name}')"
[[ -z "$(dhcp_uuids "$SWITCH_B")" ]] || die "switch B 삭제 뒤 DHCP orphan이 남았다"
for switch_name in "$SWITCH_A" "$SWITCH_B"; do
    [[ -z "$(ovn-nbctl --if-exists get Logical_Switch "$switch_name" name 2>/dev/null)" ]] ||
        die "최종 logical switch residue가 남았다: $switch_name"
done
log "PASS — 제품-only switch/DHCP cleanup과 다른 switch 보존 완료"
