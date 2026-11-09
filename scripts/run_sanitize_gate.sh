#!/usr/bin/env bash
                          
                                                                             
                                             
                                                                        
                                                                   
                                                             
                   
 
                      
                                                           
                                                          
                                                               
 
                                                             
                                                   
                                                   
                                                       
                                                
 
                                                                     
                                                                   
 
                                                                                            
                                                                                    
                                                                   
                                                                             
                                                                                
 
                                                                    
                                                                 
                                                                                    
                                                                 
                                                      
                                                              

set -uo pipefail

ROOT="${PCV_SANITIZE_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
SAN_FLAGS="${PCV_SANITIZE_FLAGS:--fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all -O1 -g -U_FORTIFY_SOURCE}"
ASAN_OPTIONS_VALUE="${PCV_SANITIZE_ASAN_OPTIONS:-detect_leaks=0:abort_on_error=0:halt_on_error=0:print_summary=1}"
UBSAN_OPTIONS_VALUE="${PCV_SANITIZE_UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}"
                                                                               
COVER_PATHS="${PCV_SANITIZE_COVER_PATHS-/var/lib/purecvisor /var/log/purecvisor /etc/purecvisor /run/purecvisor /var/log/suricata /run/libvirt /run/openvswitch /var/lib/lxc /var/lib/libvirt/images}"
REPORT="$ROOT/sanitize_report.txt"
REPORT_TMP="$(mktemp "${TMPDIR:-/tmp}/pcv-sanitize-report.XXXXXX")" || exit 1
SANITIZER_STATUS=0
SECCOMP_STATUS=0
RESTORE_STATUS=0
RESTORE_COMPLETE=0
REPORT_STATUS=0
DISPOSABLE_TREE=0
RESULT_NOTE=""

if ! cd "$ROOT"; then
    rm -f -- "$REPORT_TMP"
    exit 1
fi
: >"$REPORT_TMP"
                                                                        
                                                
                                                                                   
exec {CALLER_STDOUT}>&1 {CALLER_STDERR}>&2

                                                                     
                                                             
run_root_in_private_ns()
{
    sudo env PCV_NS_COVER="$COVER_PATHS" \
        unshare --mount --propagation private -- bash -c '
        set -u
        for p in $PCV_NS_COVER; do
            [ -d "$p" ] || continue
            mount -t tmpfs tmpfs "$p" || { echo "[sanitize] ns: cover failed: $p" >&2; exit 97; }
            if [ "$p" = /run/libvirt ]; then mkdir -p /run/libvirt/qemu || exit 97; fi
        done
        if [ -d /sys/fs/bpf ]; then
            mount -t bpf bpf /sys/fs/bpf || { echo "[sanitize] ns: bpffs mount failed" >&2; exit 97; }
        fi
        exec "$@"
    ' pcv-ns "$@"
}

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
        make single || RESTORE_STATUS=$?
    fi
    if [[ "$RESTORE_STATUS" -eq 0 ]]; then
        make test_runner || RESTORE_STATUS=$?
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

    exec 1>&"$CALLER_STDOUT" 2>&"$CALLER_STDERR"
    trap - EXIT
                                                            
                                  
    trap '' INT TERM
    if [[ "$RESTORE_COMPLETE" -ne 1 ]]; then
        if [[ "$DISPOSABLE_TREE" -eq 1 ]]; then
            [[ -n "$RESULT_NOTE" ]] || RESULT_NOTE=" (disposable tree: restore skipped after interrupt)"
        else
            record "[sanitize] finalizer: interrupted/early exit — restoring normal artifacts"
            restore_normal_artifacts || true
        fi
    fi

    record "[sanitize] result: instrumented=$SANITIZER_STATUS seccomp=$SECCOMP_STATUS restore=$RESTORE_STATUS$RESULT_NOTE"
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

if [[ "${PCV_SANITIZE_DISPOSABLE_TREE:-0}" == "1" ]]; then
    if [[ -e "$ROOT/.git" ]]; then
        record "[sanitize] PCV_SANITIZE_DISPOSABLE_TREE ignored: repository checkout must restore normal artifacts"
    else
        DISPOSABLE_TREE=1
    fi
fi

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
    run_root_in_private_ns env -u LSAN_OPTIONS \
        ASAN_OPTIONS="$ASAN_OPTIONS_VALUE" \
        UBSAN_OPTIONS="$UBSAN_OPTIONS_VALUE" \
        ./test_runner -v -x /privdrop/seccomp_subprocess \
        >>"$REPORT_TMP" 2>&1 {CALLER_STDOUT}>&- {CALLER_STDERR}>&-
    SANITIZER_STATUS=$?
else
    record "[sanitize] instrumented build failed (status=$SANITIZER_STATUS)"
fi

                                                                      
if [[ "$DISPOSABLE_TREE" -eq 1 && "$SANITIZER_STATUS" -ne 0 ]]; then
    RESULT_NOTE=" (disposable tree: seccomp phase and restore skipped after instrumented failure)"
    exit 1
fi

                                                       
                                                         
restore_normal_artifacts || true

if [[ "$RESTORE_STATUS" -eq 0 ]]; then
    record "[sanitize] phase 3/3: normal seccomp subprocess"
    run_root_in_private_ns env -u ASAN_OPTIONS -u UBSAN_OPTIONS -u LSAN_OPTIONS \
        ./test_runner -p /privdrop/seccomp_subprocess \
        >>"$REPORT_TMP" 2>&1 {CALLER_STDOUT}>&- {CALLER_STDERR}>&-
    SECCOMP_STATUS=$?

else
    SECCOMP_STATUS=1
fi

if [[ "$SANITIZER_STATUS" -ne 0 || "$SECCOMP_STATUS" -ne 0 || "$RESTORE_STATUS" -ne 0 ]]; then
    exit 1
fi

exit 0
