#!/usr/bin/env bash










set -euo pipefail

readonly MEMORY_PER_JOB_MIB=512

usage() {
    printf 'Usage: %s [--report]\n' "${0##*/}" >&2
}

fail_input() {
    printf 'dev_build_jobs: %s\n' "$1" >&2
    exit 2
}

require_positive_integer() {
    local name="$1"
    local value="$2"

    case "$value" in
        ''|*[!0-9]*) fail_input "$name must be a positive integer" ;;
    esac
    if [ "$value" -lt 1 ]; then
        fail_input "$name must be a positive integer"
    fi
}

read_logical_cpus() {
    local value="${PCV_BUILD_TEST_LOGICAL:-}"

    if [ -z "$value" ]; then
        if command -v nproc >/dev/null 2>&1; then
            value="$(nproc)"
        else
            value="$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '1')"
        fi
    fi
    require_positive_integer "logical CPU count" "$value"
    printf '%s\n' "$value"
}

read_physical_cores() {
    local logical="$1"
    local value="${PCV_BUILD_TEST_PHYSICAL:-}"

    if [ -z "$value" ] && command -v lscpu >/dev/null 2>&1; then
        value="$(lscpu -p=CORE,SOCKET 2>/dev/null \
            | awk -F, '$1 !~ /^#/ {print $1 "," $2}' \
            | sort -u \
            | wc -l)"
        value="${value//[[:space:]]/}"
    fi
    if [ -z "$value" ]; then
        value="$logical"
    fi
    require_positive_integer "physical core count" "$value"
    if [ "$value" -gt "$logical" ]; then
        value="$logical"
    fi
    printf '%s\n' "$value"
}

read_available_memory_mib() {
    local logical="$1"
    local value="${PCV_BUILD_TEST_MEM_AVAILABLE_MIB:-}"

    if [ -z "$value" ] && [ -r /proc/meminfo ]; then
        value="$(awk '/^MemAvailable:/ {print int($2 / 1024); exit}' /proc/meminfo)"
    fi


    if [ -z "$value" ]; then
        value="$((logical * MEMORY_PER_JOB_MIB))"
    fi
    require_positive_integer "available memory MiB" "$value"
    printf '%s\n' "$value"
}

report=false
case "$#" in
    0) ;;
    1)
        if [ "$1" != "--report" ]; then
            usage
            exit 2
        fi
        report=true
        ;;
    *)
        usage
        exit 2
        ;;
esac

override="${PCV_BUILD_JOBS:-}"
if [ -n "$override" ]; then
    require_positive_integer "PCV_BUILD_JOBS" "$override"
fi

logical="$(read_logical_cpus)"
physical="$(read_physical_cores "$logical")"
available_memory_mib="$(read_available_memory_mib "$logical")"
memory_job_limit="$((available_memory_mib / MEMORY_PER_JOB_MIB))"
if [ "$memory_job_limit" -lt 1 ]; then
    memory_job_limit=1
fi

smt_threads="$((logical - physical))"
cpu_job_target="$((physical + (smt_threads * 2 / 3)))"
if [ "$cpu_job_target" -lt 1 ]; then
    cpu_job_target=1
elif [ "$cpu_job_target" -gt "$logical" ]; then
    cpu_job_target="$logical"
fi

recommended="$cpu_job_target"
if [ "$memory_job_limit" -lt "$recommended" ]; then
    recommended="$memory_job_limit"
fi
source=automatic
if [ -n "$override" ]; then
    recommended="$override"
    source=override
fi

if $report; then
    printf 'logical_cpus=%s\n' "$logical"
    printf 'physical_cores=%s\n' "$physical"
    printf 'available_memory_mib=%s\n' "$available_memory_mib"
    printf 'memory_per_job_mib=%s\n' "$MEMORY_PER_JOB_MIB"
    printf 'memory_job_limit=%s\n' "$memory_job_limit"
    printf 'cpu_job_target=%s\n' "$cpu_job_target"
    printf 'recommended_jobs=%s\n' "$recommended"
    printf 'selection_source=%s\n' "$source"
else
    printf '%s\n' "$recommended"
fi
