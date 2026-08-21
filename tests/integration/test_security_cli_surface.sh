#!/usr/bin/env bash
                                                                                   
                                                             
                                                           
set -euo pipefail

                                                        
                                                                            

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

if [[ ! -x bin/pcvctl ]]; then
    echo "missing bin/pcvctl; run make cli first" >&2
    exit 1
fi

help_out="$(bin/pcvctl --no-color help security 2>&1)"
for action in \
    status \
    events \
    event \
    pending \
    approve \
    dismiss \
    baseline-status \
    baseline-refresh \
    enable \
    disable
do
    if ! grep -Eq "^[[:space:]]*pcvctl[[:space:]]+security[[:space:]]+$action[[:space:]]" <<<"$help_out"; then
        echo "missing CLI security command in help: security $action" >&2
        exit 1
    fi
done

echo "security CLI surface OK"
