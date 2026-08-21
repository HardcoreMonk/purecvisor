#!/usr/bin/env bash
                                                                                       
                                                                             
                                                                 

set -uo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
SCRIPT="${ROOT_DIR}/scripts/wait-for-local-ip.sh"
TMP_DIR=$(mktemp -d)
trap 'rm -rf -- "${TMP_DIR}"' EXIT

failures=0

fail() {
    printf 'not ok - %s\n' "$1" >&2
    failures=$((failures + 1))
}

run_case() {
    local name=$1 expected=$2
    shift 2
    local stdout_file="${TMP_DIR}/${name}.stdout"
    local stderr_file="${TMP_DIR}/${name}.stderr"
    local status

    "$@" >"${stdout_file}" 2>"${stderr_file}"
    status=$?
    if [[ ${status} -ne ${expected} ]]; then
        fail "${name}: expected exit ${expected}, got ${status}"
        return
    fi
    if [[ -s ${stdout_file} ]]; then
        fail "${name}: stdout must remain empty"
        return
    fi
    if grep -Fq '192.0.2.73' "${stderr_file}" ||
       grep -Fq 'DO_NOT_LEAK' "${stderr_file}"; then
        fail "${name}: diagnostics leaked an address or environment value"
        return
    fi
    printf 'ok - %s\n' "${name}"
}

if [[ ! -x ${SCRIPT} ]]; then
    printf 'not ok - wait-for-local-ip script is missing or not executable\n' >&2
    exit 1
fi

FAKE_IP="${TMP_DIR}/ip"
cat >"${FAKE_IP}" <<'EOF'
#!/usr/bin/env bash
set -u
[[ $# -eq 4 && $1 == -4 && $2 == -o && $3 == address && $4 == show ]] || exit 64
case "${FAKE_IP_MODE:-}" in
    present)
        printf '2: eth0 inet 192.0.2.73/24 brd 192.0.2.255 scope global eth0\n'
        ;;
    delayed)
        count=0
        if [[ -f ${FAKE_IP_COUNT_FILE} ]]; then
            read -r count <"${FAKE_IP_COUNT_FILE}"
        fi
        count=$((count + 1))
        printf '%s\n' "${count}" >"${FAKE_IP_COUNT_FILE}"
        if [[ ${count} -ge 3 ]]; then
            printf '2: eth0 inet 192.0.2.73/24 scope global eth0\n'
        fi
        ;;
    absent)
        printf '2: eth0 inet 192.0.2.7/24 scope global eth0\n'
        ;;
    later_inet_decoy)
        printf '7: veth@if9 inet 192.0.2.7/24 brd inet 192.0.2.73/24 scope global veth@if9\n'
        ;;
    slow_absent)
        count=0
        if [[ -f ${FAKE_IP_COUNT_FILE} ]]; then
            read -r count <"${FAKE_IP_COUNT_FILE}"
        fi
        printf '%s\n' "$((count + 1))" >"${FAKE_IP_COUNT_FILE}"
        /bin/sleep 0.2
        printf '2: eth0 inet 192.0.2.7/24 scope global eth0\n'
        ;;
    failure)
        exit 42
        ;;
esac
EOF
chmod +x "${FAKE_IP}"

FAKE_SLEEP="${TMP_DIR}/sleep"
cat >"${FAKE_SLEEP}" <<'EOF'
#!/usr/bin/env bash
set -u
[[ $# -eq 1 ]] || exit 64
if [[ -n ${FAKE_SLEEP_LOG:-} ]]; then
    printf '%s\n' "$1" >>"${FAKE_SLEEP_LOG}"
fi
if [[ ${FAKE_SLEEP_MODE:-success} != success ]]; then
    printf '%s\n' "${PCV_WAIT_FOR_LOCAL_IP_TEST_SECRET:-}" >&2
    exit 1
fi
/bin/sleep 0.01
EOF
chmod +x "${FAKE_SLEEP}"

COMMON_ENV=(
    env
    "PCV_WAIT_FOR_LOCAL_IP_IP_COMMAND=${FAKE_IP}"
    "PCV_WAIT_FOR_LOCAL_IP_POLL_SECONDS=0.01"
    "PCV_WAIT_FOR_LOCAL_IP_SLEEP_COMMAND=${FAKE_SLEEP}"
    "PCV_WAIT_FOR_LOCAL_IP_TEST_SECRET=DO_NOT_LEAK"
)

run_case present 0 "${COMMON_ENV[@]}" FAKE_IP_MODE=present \
    "${SCRIPT}" 192.0.2.73 1

COUNT_FILE="${TMP_DIR}/delayed.count"
run_case delayed 0 "${COMMON_ENV[@]}" FAKE_IP_MODE=delayed \
    "FAKE_IP_COUNT_FILE=${COUNT_FILE}" "${SCRIPT}" 192.0.2.73 2

run_case timeout 75 "${COMMON_ENV[@]}" FAKE_IP_MODE=absent \
    "${SCRIPT}" 192.0.2.73 0
run_case later_inet_decoy 75 "${COMMON_ENV[@]}" FAKE_IP_MODE=later_inet_decoy \
    "${SCRIPT}" 192.0.2.73 0

run_case invalid 2 "${COMMON_ENV[@]}" FAKE_IP_MODE=present \
    "${SCRIPT}" 999.168.3.73 1
run_case multiple 2 "${COMMON_ENV[@]}" FAKE_IP_MODE=present \
    "${SCRIPT}" 192.0.2.73 192.0.2.74 1
run_case ipv6 2 "${COMMON_ENV[@]}" FAKE_IP_MODE=present \
    "${SCRIPT}" 2001:db8::1 1
run_case command_failure 1 "${COMMON_ENV[@]}" FAKE_IP_MODE=failure \
    "${SCRIPT}" 192.0.2.73 1

run_case leading_zero_timeout 0 "${COMMON_ENV[@]}" FAKE_IP_MODE=present \
    "${SCRIPT}" 192.0.2.73 08
run_case timeout_overflow 2 "${COMMON_ENV[@]}" FAKE_IP_MODE=present \
    "${SCRIPT}" 192.0.2.73 999999999999999999999999999999

for zero_poll in .0 00 0.00; do
    run_case "zero_poll_${zero_poll//./_}" 2 env \
        "PCV_WAIT_FOR_LOCAL_IP_IP_COMMAND=${FAKE_IP}" \
        "PCV_WAIT_FOR_LOCAL_IP_POLL_SECONDS=${zero_poll}" \
        "PCV_WAIT_FOR_LOCAL_IP_SLEEP_COMMAND=${FAKE_SLEEP}" \
        FAKE_IP_MODE=present "${SCRIPT}" 192.0.2.73 1
done

run_case huge_poll 2 env \
    "PCV_WAIT_FOR_LOCAL_IP_IP_COMMAND=${FAKE_IP}" \
    "PCV_WAIT_FOR_LOCAL_IP_POLL_SECONDS=999999999999999999999999.0" \
    "PCV_WAIT_FOR_LOCAL_IP_SLEEP_COMMAND=${FAKE_SLEEP}" \
    FAKE_IP_MODE=present "${SCRIPT}" 192.0.2.73 1

SLEEP_LOG="${TMP_DIR}/sleep-cap.log"
run_case poll_capped_to_deadline 75 env \
    "PCV_WAIT_FOR_LOCAL_IP_IP_COMMAND=${FAKE_IP}" \
    "PCV_WAIT_FOR_LOCAL_IP_POLL_SECONDS=60" \
    "PCV_WAIT_FOR_LOCAL_IP_SLEEP_COMMAND=${FAKE_SLEEP}" \
    "FAKE_SLEEP_LOG=${SLEEP_LOG}" FAKE_IP_MODE=absent \
    "${SCRIPT}" 192.0.2.73 1
first_sleep=$(head -n 1 "${SLEEP_LOG}" 2>/dev/null)
if ! awk -v value="${first_sleep}" \
    'BEGIN { exit !(value > 0 && value <= 1) }'; then
    fail "poll_capped_to_deadline: sleep exceeded the remaining deadline"
fi

run_case sleep_failure 1 env \
    "PCV_WAIT_FOR_LOCAL_IP_IP_COMMAND=${FAKE_IP}" \
    "PCV_WAIT_FOR_LOCAL_IP_POLL_SECONDS=0.01" \
    "PCV_WAIT_FOR_LOCAL_IP_SLEEP_COMMAND=${FAKE_SLEEP}" \
    "PCV_WAIT_FOR_LOCAL_IP_TEST_SECRET=DO_NOT_LEAK" \
    FAKE_SLEEP_MODE=failure FAKE_IP_MODE=absent \
    "${SCRIPT}" 192.0.2.73 1

SLOW_COUNT_FILE="${TMP_DIR}/slow.count"
SLOW_SLEEP_LOG="${TMP_DIR}/slow-sleep.log"
run_case fractional_remaining 75 env \
    "PCV_WAIT_FOR_LOCAL_IP_IP_COMMAND=${FAKE_IP}" \
    "PCV_WAIT_FOR_LOCAL_IP_POLL_SECONDS=60" \
    "PCV_WAIT_FOR_LOCAL_IP_SLEEP_COMMAND=${FAKE_SLEEP}" \
    "FAKE_IP_COUNT_FILE=${SLOW_COUNT_FILE}" \
    "FAKE_SLEEP_LOG=${SLOW_SLEEP_LOG}" FAKE_IP_MODE=slow_absent \
    "${SCRIPT}" 192.0.2.73 1
if grep -Eq '^0([.]0*)?$' "${SLOW_SLEEP_LOG}" 2>/dev/null; then
    fail "fractional_remaining: invoked sleep with zero"
fi
slow_probe_count=$(<"${SLOW_COUNT_FILE}")
if ((slow_probe_count > 10)); then
    fail "fractional_remaining: probe count indicates a tight loop"
fi

if [[ ${failures} -ne 0 ]]; then
    exit 1
fi
