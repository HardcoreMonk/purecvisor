#!/usr/bin/env bash
                          
                                                                             
                                             
                                                                        
                                                                   
                                                             
                   
 
                      
                                                           
                                                          
                                                               
 
                                                             
                                                   
                                                   
                                                       
                                                
 
                                                                     
                                                                   

set -uo pipefail

ROOT="${PCV_SANITIZE_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
SAN_FLAGS="${PCV_SANITIZE_FLAGS:--fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all -O1 -g -U_FORTIFY_SOURCE}"
ASAN_OPTIONS_VALUE="${PCV_SANITIZE_ASAN_OPTIONS:-detect_leaks=0:abort_on_error=0:halt_on_error=0:print_summary=1}"
UBSAN_OPTIONS_VALUE="${PCV_SANITIZE_UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}"
REPORT="$ROOT/sanitize_report.txt"
REPORT_TMP="$(mktemp "${TMPDIR:-/tmp}/pcv-sanitize-report.XXXXXX")" || exit 1
SANITIZER_STATUS=0
SECCOMP_STATUS=0
RESTORE_STATUS=0
RESTORE_COMPLETE=0
REPORT_STATUS=0

if ! cd "$ROOT"; then
    rm -f -- "$REPORT_TMP"
    exit 1
fi
: >"$REPORT_TMP"

record()
{
    printf '%s\n' "$*" | tee -a "$REPORT_TMP"
}

restore_normal_artifacts()
{
    local ldd_output=""

                                                                
                                                      
    RESTORE_STATUS=0
    record "[sanitize] phase 2/3: restore normal single + test_runner"
    make clean || RESTORE_STATUS=$?
    if [[ "$RESTORE_STATUS" -eq 0 ]]; then
        make single test_runner || RESTORE_STATUS=$?
    fi

    if [[ "$RESTORE_STATUS" -eq 0 ]]; then
        [[ -x ./bin/purecvisorsd && -x ./bin/pcvctl && -x ./test_runner ]] \
            || RESTORE_STATUS=1
    fi
    if [[ "$RESTORE_STATUS" -eq 0 ]]; then
        if ! ldd_output="$(ldd ./bin/purecvisorsd ./bin/pcvctl ./test_runner 2>&1)"; then
            record "[sanitize] restored artifact dynamic-link inspection failed"
            RESTORE_STATUS=1
        elif grep -Eq 'libasan|libubsan' <<<"$ldd_output"; then
            record "[sanitize] restored artifacts still link sanitizer runtime"
            RESTORE_STATUS=1
        fi
    fi
    if [[ "$RESTORE_STATUS" -ne 0 ]]; then
        record "[sanitize] normal artifact restore failed (status=$RESTORE_STATUS)"
    fi
    RESTORE_COMPLETE=1
    return "$RESTORE_STATUS"
}

finalize()
{
    local original_status=$?

    trap - EXIT
                                                            
                                  
    trap '' INT TERM
    if [[ "$RESTORE_COMPLETE" -ne 1 ]]; then
        record "[sanitize] finalizer: interrupted/early exit — restoring normal artifacts"
        restore_normal_artifacts || true
    fi

    record "[sanitize] result: instrumented=$SANITIZER_STATUS seccomp=$SECCOMP_STATUS restore=$RESTORE_STATUS"
    cp -- "$REPORT_TMP" "$REPORT" || REPORT_STATUS=$?
    if [[ -f "$REPORT" ]]; then
        cat "$REPORT" || REPORT_STATUS=$?
    fi
    rm -f -- "$REPORT_TMP"

    if [[ "$original_status" -eq 0 \
          && ( "$RESTORE_STATUS" -ne 0 || "$REPORT_STATUS" -ne 0 ) ]]; then
        exit 1
    fi
    exit "$original_status"
}

trap 'exit 130' INT
trap 'exit 143' TERM
trap finalize EXIT

record "[sanitize] phase 1/3: ASan/UBSan build"
make clean || SANITIZER_STATUS=$?
if [[ "$SANITIZER_STATUS" -eq 0 ]]; then
    make CFLAGS_EXTRA="$SAN_FLAGS" LDFLAGS_EXTRA="$SAN_FLAGS" test_runner \
        || SANITIZER_STATUS=$?
fi

if [[ "$SANITIZER_STATUS" -eq 0 ]]; then
    record "[sanitize] phase 1/3: instrumented suite (normal seccomp path excluded)"
    sudo env -u LSAN_OPTIONS \
        ASAN_OPTIONS="$ASAN_OPTIONS_VALUE" \
        UBSAN_OPTIONS="$UBSAN_OPTIONS_VALUE" \
        ./test_runner -v -x /privdrop/seccomp_subprocess \
        >>"$REPORT_TMP" 2>&1
    SANITIZER_STATUS=$?
else
    record "[sanitize] instrumented build failed (status=$SANITIZER_STATUS)"
fi

                                                       
                                                         
restore_normal_artifacts || true

if [[ "$RESTORE_STATUS" -eq 0 ]]; then
    record "[sanitize] phase 3/3: normal seccomp subprocess"
    sudo env -u ASAN_OPTIONS -u UBSAN_OPTIONS -u LSAN_OPTIONS \
        ./test_runner -p /privdrop/seccomp_subprocess \
        >>"$REPORT_TMP" 2>&1
    SECCOMP_STATUS=$?

else
    SECCOMP_STATUS=1
fi

if [[ "$SANITIZER_STATUS" -ne 0 || "$SECCOMP_STATUS" -ne 0 || "$RESTORE_STATUS" -ne 0 ]]; then
    exit 1
fi

exit 0
