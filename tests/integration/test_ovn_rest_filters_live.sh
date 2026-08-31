#!/usr/bin/env bash













set -Eeuo pipefail

readonly SOCKET="${PCV_OVN_REST_SOCKET:-/var/run/purecvisor/daemon.sock}"
readonly REST_BASE="${PCV_OVN_REST_BASE:-https://127.0.0.1/api/v1}"
readonly RUN_TOKEN="${PCV_OVN_REST_RUN_ID:-$(date +%H%M%S)$$}"
readonly SWITCH_A="pcv-rest-a-${RUN_TOKEN}"
readonly SWITCH_B="pcv-rest-b-${RUN_TOKEN}"
readonly ROUTER_A="pcv-rest-ra-${RUN_TOKEN}"
readonly ROUTER_B="pcv-rest-rb-${RUN_TOKEN}"
readonly TEST_USER="pcvovn_${RUN_TOKEN,,}"
readonly TEST_PASSWORD="Pcv!Ovn-${RUN_TOKEN}-9aA"
readonly ACL_MARK_A="ip4.src == 10.254.61.0/24"
readonly ACL_MARK_B="ip4.src == 10.254.62.0/24"
readonly NAT_MARK_A="10.254.61.0/24"
readonly NAT_MARK_B="10.254.62.0/24"
readonly NAT_EXT_A="192.0.2.61"
readonly NAT_EXT_B="192.0.2.62"
USER_CREATED=0
EMERGENCY_CLEANUP=0

log() { printf '[OVN-REST-FILTER] %s\n' "$*"; }
die() { log "FAIL — $*" >&2; exit 1; }

rpc_allow_error()
{
    local method="$1" params="$2"
    printf '{"jsonrpc":"2.0","method":"%s","params":%s,"id":1}\n' \
        "$method" "$params" | socat -t 12 - UNIX-CONNECT:"$SOCKET"
}

rpc()
{
    local method="$1" params="$2" response
    response="$(rpc_allow_error "$method" "$params")"
    if ! jq -e '.result != null and .error == null' >/dev/null <<<"$response"; then
        log "RPC 실패: ${method}"
        jq -c '{error}' <<<"$response" >&2 || true
        return 1
    fi
    printf '%s\n' "$response"
}

resource_exists()
{
    local table="$1" name="$2"
    ovn-nbctl --timeout=5 --if-exists get "$table" "$name" name 2>/dev/null | grep -q .
}

cleanup()
{
    local exit_code=$?
    trap - EXIT INT TERM
    set +e

    rpc_allow_error ovn.router.delete "{\"name\":\"${ROUTER_A}\"}" >/dev/null 2>&1
    rpc_allow_error ovn.router.delete "{\"name\":\"${ROUTER_B}\"}" >/dev/null 2>&1
    rpc_allow_error ovn.switch.delete "{\"name\":\"${SWITCH_A}\"}" >/dev/null 2>&1
    rpc_allow_error ovn.switch.delete "{\"name\":\"${SWITCH_B}\"}" >/dev/null 2>&1
    if [[ "$USER_CREATED" -eq 1 ]]; then
        rpc_allow_error auth.user.delete "{\"username\":\"${TEST_USER}\"}" >/dev/null 2>&1
    fi

    for spec in \
        "Logical_Router:${ROUTER_A}" "Logical_Router:${ROUTER_B}" \
        "Logical_Switch:${SWITCH_A}" "Logical_Switch:${SWITCH_B}"; do
        table="${spec%%:*}"
        name="${spec#*:}"
        if resource_exists "$table" "$name"; then
            if [[ "$table" == "Logical_Router" ]]; then
                ovn-nbctl --timeout=5 --if-exists lr-del "$name" >/dev/null 2>&1
            else
                ovn-nbctl --timeout=5 --if-exists ls-del "$name" >/dev/null 2>&1
            fi
            EMERGENCY_CLEANUP=$((EMERGENCY_CLEANUP + 1))
        fi
    done

    if [[ "$exit_code" -eq 0 && "$EMERGENCY_CLEANUP" -ne 0 ]]; then
        log "FAIL — 정상 경로에서 exact emergency cleanup ${EMERGENCY_CLEANUP}건 실행" >&2
        exit_code=1
    elif [[ "$EMERGENCY_CLEANUP" -ne 0 ]]; then
        log "cleanup — 실패 뒤 exact OVN parent ${EMERGENCY_CLEANUP}건 emergency 제거"
    fi
    if [[ "$exit_code" -eq 0 ]]; then
        log "PASS — 제품 RPC cleanup과 시험 자원 residue 0"
    fi
    exit "$exit_code"
}
trap cleanup EXIT INT TERM

[[ "$EUID" -eq 0 ]] || die "root 권한이 필요하다"
[[ "${PCV_OVN_REST_FILTERS_LIVE:-}" == "1" ]] || \
    die "PCV_OVN_REST_FILTERS_LIVE=1 명시 opt-in이 필요하다"
[[ "$RUN_TOKEN" =~ ^[A-Za-z0-9]{1,12}$ ]] || \
    die "PCV_OVN_REST_RUN_ID는 영숫자 1~12자여야 한다"
[[ ${#TEST_USER} -le 32 ]] || die "시험 사용자 이름이 32자를 넘는다"
for command_name in curl jq ovn-nbctl socat systemctl; do
    command -v "$command_name" >/dev/null || die "필수 명령이 없다: $command_name"
done
[[ -S "$SOCKET" ]] || die "제품 UDS가 없다: $SOCKET"
systemctl is-active --quiet purecvisorsd.service || die "purecvisorsd가 active가 아니다"

STATUS="$(rpc ovn.status '{}')"
jq -e '.result.available == true' >/dev/null <<<"$STATUS" || die "제품 OVN이 available이 아니다"
for spec in \
    "Logical_Switch:${SWITCH_A}" "Logical_Switch:${SWITCH_B}" \
    "Logical_Router:${ROUTER_A}" "Logical_Router:${ROUTER_B}"; do
    resource_exists "${spec%%:*}" "${spec#*:}" && die "시험 OVN 이름이 이미 존재한다: ${spec#*:}"
done
USERS="$(rpc auth.user.list '{}')"
jq -e --arg username "$TEST_USER" \
    '.result | all(.[]; .username != $username)' >/dev/null <<<"$USERS" || \
    die "시험 사용자가 이미 존재한다: $TEST_USER"
log "PASS 1 — service, OVN, REST와 run-unique 이름 preflight"

rpc auth.user.create \
    "{\"username\":\"${TEST_USER}\",\"password\":\"${TEST_PASSWORD}\",\"role\":\"viewer\"}" \
    >/dev/null
USER_CREATED=1

rpc ovn.switch.create "{\"name\":\"${SWITCH_A}\"}" >/dev/null
rpc ovn.switch.create "{\"name\":\"${SWITCH_B}\"}" >/dev/null
rpc ovn.router.create "{\"name\":\"${ROUTER_A}\"}" >/dev/null
rpc ovn.router.create "{\"name\":\"${ROUTER_B}\"}" >/dev/null
rpc ovn.acl.add \
    "{\"switch\":\"${SWITCH_A}\",\"direction\":\"to-lport\",\"priority\":1761,\"match\":\"${ACL_MARK_A}\",\"action\":\"allow\"}" \
    >/dev/null
rpc ovn.acl.add \
    "{\"switch\":\"${SWITCH_B}\",\"direction\":\"to-lport\",\"priority\":1762,\"match\":\"${ACL_MARK_B}\",\"action\":\"drop\"}" \
    >/dev/null
rpc ovn.nat.add \
    "{\"router\":\"${ROUTER_A}\",\"type\":\"snat\",\"external_ip\":\"${NAT_EXT_A}\",\"logical_ip\":\"${NAT_MARK_A}\"}" \
    >/dev/null
rpc ovn.nat.add \
    "{\"router\":\"${ROUTER_B}\",\"type\":\"snat\",\"external_ip\":\"${NAT_EXT_B}\",\"logical_ip\":\"${NAT_MARK_B}\"}" \
    >/dev/null
log "PASS 2 — 독립 ACL/NAT 표식 두 세트와 임시 VIEWER 생성"

CURL_TLS=()
if [[ "${PCV_OVN_REST_INSECURE:-1}" == "1" ]]; then
    CURL_TLS=(-k)
fi
LOGIN_RESPONSE="$(
    printf '{"username":"%s","password":"%s"}' "$TEST_USER" "$TEST_PASSWORD" | \
        curl "${CURL_TLS[@]}" -sS --fail-with-body --max-time 12 \
            -H 'Content-Type: application/json' --data-binary @- \
            "${REST_BASE}/auth/token"
)"
TOKEN="$(jq -er '.access_token // empty' <<<"$LOGIN_RESPONSE")" || \
    die "임시 VIEWER access token을 발급하지 못했다"
[[ -n "$TOKEN" ]] || die "임시 VIEWER access token이 비어 있다"
log "PASS 3 — 임시 VIEWER JWT 인증"

rest_get()
{
    local path="$1"
    curl "${CURL_TLS[@]}" -sS --fail-with-body --max-time 12 \
        -H "Authorization: Bearer ${TOKEN}" "${REST_BASE}${path}"
}

ACL_A_JSON="$(rest_get "/ovn/acl?switch=${SWITCH_A}")"
ACL_B_JSON="$(rest_get "/ovn/acl?switch=${SWITCH_B}")"
NAT_A_JSON="$(rest_get "/ovn/nat?router=${ROUTER_A}")"
NAT_B_JSON="$(rest_get "/ovn/nat?router=${ROUTER_B}")"

jq -e --arg own "$ACL_MARK_A" --arg foreign "$ACL_MARK_B" \
    '.data | type == "array" and any(.[]; contains($own)) and all(.[]; contains($foreign) | not)' \
    >/dev/null <<<"$ACL_A_JSON" || die "ACL switch A filter가 대상 포함/비대상 제외를 만족하지 않는다"
jq -e --arg own "$ACL_MARK_B" --arg foreign "$ACL_MARK_A" \
    '.data | type == "array" and any(.[]; contains($own)) and all(.[]; contains($foreign) | not)' \
    >/dev/null <<<"$ACL_B_JSON" || die "ACL switch B filter가 대상 포함/비대상 제외를 만족하지 않는다"
jq -e --arg own "$NAT_MARK_A" --arg foreign "$NAT_MARK_B" \
    '.data | type == "array" and any(.[]; contains($own)) and all(.[]; contains($foreign) | not)' \
    >/dev/null <<<"$NAT_A_JSON" || die "NAT router A filter가 대상 포함/비대상 제외를 만족하지 않는다"
jq -e --arg own "$NAT_MARK_B" --arg foreign "$NAT_MARK_A" \
    '.data | type == "array" and any(.[]; contains($own)) and all(.[]; contains($foreign) | not)' \
    >/dev/null <<<"$NAT_B_JSON" || die "NAT router B filter가 대상 포함/비대상 제외를 만족하지 않는다"
log "PASS 4 — 인증 REST ACL/NAT query의 양성·음성 filter 분리"
