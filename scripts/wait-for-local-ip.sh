#!/usr/bin/env bash
                          
                                                                                       
                                                    
                                                                                                 
 
                      
                                                

                                                                             
 
     
                                                 
                                                              
                                                                
                                                                                
                                                      
 
                  
                                             
                                              
                                            
                                                      
                                                           
                                                                 
 
                                      
 
            
                                      
                                                          
                                 
                                                        
 
                
                                                                    
                                                                 
                                                         
                                                    
                                                      
                                   

set -uo pipefail
export LC_ALL=C

readonly EX_USAGE=2
readonly EX_FAILURE=1
readonly EX_TEMPFAIL=75
                                                            
readonly MAX_TIMEOUT_SECONDS=3600
readonly MAX_POLL_SECONDS=60
readonly MIN_POLL_SECONDS=0.000000001

usage_error() {
    printf 'wait-for-local-ip: invalid arguments\n' >&2
    exit "${EX_USAGE}"
}

is_ipv4() {
    local address=$1
    local octet
    local -a octets

    [[ ${address} =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}$ ]] || return 1
    IFS=. read -r -a octets <<<"${address}"
    for octet in "${octets[@]}"; do
        [[ ${octet} == "0" || ${octet} != 0* ]] || return 1
        ((10#${octet} <= 255)) || return 1
    done
}

[[ $# -ge 1 && $# -le 2 ]] || usage_error

readonly target_address=$1
readonly timeout_input=${2:-60}
readonly poll_input=${PCV_WAIT_FOR_LOCAL_IP_POLL_SECONDS:-1}
readonly ip_command=${PCV_WAIT_FOR_LOCAL_IP_IP_COMMAND:-ip}
readonly sleep_command=${PCV_WAIT_FOR_LOCAL_IP_SLEEP_COMMAND:-sleep}

is_ipv4 "${target_address}" || usage_error
[[ ${timeout_input} =~ ^[0-9]+$ && ${#timeout_input} -le 4 ]] || usage_error
timeout_seconds=$((10#${timeout_input}))
((timeout_seconds <= MAX_TIMEOUT_SECONDS)) || usage_error
readonly timeout_seconds

[[ ${poll_input} =~ ^([0-9]+([.][0-9]+)?|[.][0-9]+)$ &&
   ${#poll_input} -le 16 ]] || usage_error
if ! poll_seconds=$(
    awk -v value="${poll_input}" -v minimum="${MIN_POLL_SECONDS}" \
        -v maximum="${MAX_POLL_SECONDS}" \
        'BEGIN {
            value += 0
            if (value < minimum || value > maximum) {
                exit 1
            }
            printf "%.9g", value
        }'
); then
    usage_error
fi
readonly poll_seconds

started_at=
if ! read -r started_at _ </proc/uptime; then
    printf 'wait-for-local-ip: monotonic clock unavailable\n' >&2
    exit "${EX_FAILURE}"
fi
deadline=$(
    awk -v start="${started_at}" -v timeout="${timeout_seconds}" \
        'BEGIN { printf "%.9f", start + timeout }'
)
readonly deadline

while :; do
    ip_output=
    if ! ip_output=$("${ip_command}" -4 -o address show 2>/dev/null); then
        printf 'wait-for-local-ip: address inspection failed\n' >&2
        exit "${EX_FAILURE}"
    fi

    while IFS= read -r line; do
        _ifindex=
        _interface_name=
        family=
        local_field=
        read -r _ifindex _interface_name family local_field _remainder <<<"${line}"
        if [[ ${family} == inet &&
              ${local_field%%/*} == "${target_address}" ]]; then
            exit 0
        fi
    done <<<"${ip_output}"

    now=
    if ! read -r now _ </proc/uptime; then
        printf 'wait-for-local-ip: monotonic clock unavailable\n' >&2
        exit "${EX_FAILURE}"
    fi
    if ! remaining_seconds=$(
        awk -v deadline="${deadline}" -v now="${now}" \
            'BEGIN {
                remaining = deadline - now
                if (remaining <= 0) {
                    exit 1
                }
                printf "%.9g", remaining
            }'
    ); then
        printf 'wait-for-local-ip: address not available before timeout\n' >&2
        exit "${EX_TEMPFAIL}"
    fi

    if ! sleep_seconds=$(
        awk -v poll="${poll_seconds}" -v remaining="${remaining_seconds}" \
            'BEGIN {
                delay = remaining
                if (poll < remaining) {
                    delay = poll
                }
                if (delay < 0.000000001) {
                    exit 1
                }
                printf "%.9f", delay
            }'
    ); then
        printf 'wait-for-local-ip: address not available before timeout\n' >&2
        exit "${EX_TEMPFAIL}"
    fi
    if ! "${sleep_command}" "${sleep_seconds}" 2>/dev/null; then
        printf 'wait-for-local-ip: polling delay failed\n' >&2
        exit "${EX_FAILURE}"
    fi
done
