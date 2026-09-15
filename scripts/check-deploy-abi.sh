#!/usr/bin/env bash





set -euo pipefail

if (( $# == 0 )); then
    printf 'Usage: %s ARTIFACT [ARTIFACT ...]\n' "${0##*/}" >&2
    exit 2
fi

if ! command -v ldd >/dev/null 2>&1; then
    printf 'error: ldd is required for deploy ABI preflight\n' >&2
    exit 1
fi

artifact_count=$#
for artifact in "$@"; do
    if [[ ! -f "$artifact" || -L "$artifact" || ! -x "$artifact" ]]; then
        printf 'error: deploy artifact must be an executable regular non-symlink: %s\n' \
            "$artifact" >&2
        exit 1
    fi

    ldd_report=""
    if ! ldd_report="$(LC_ALL=C ldd "$artifact" 2>&1)"; then
        printf 'error: target loader rejected deploy artifact: %s\n' "$artifact" >&2
        [[ -z "$ldd_report" ]] || printf '%s\n' "$ldd_report" >&2
        exit 1
    fi
    if [[ "$ldd_report" =~ (^|[[:space:]])not[[:space:]]+found($|[[:space:]]) ]]; then
        printf 'error: ABI dependency resolution failed: %s\n' "$artifact" >&2
        printf '%s\n' "$ldd_report" >&2
        exit 1
    fi
done

printf 'PCV_DEPLOY_ABI_PREFLIGHT=OK binaries=%d\n' "$artifact_count"
