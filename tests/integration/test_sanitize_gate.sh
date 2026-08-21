#!/usr/bin/env bash
                                                                                         
                                                                               
                                                                   
                                                                
 
                                                
                                                          
                                                         
                                                       
                                                         

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
GATE="$ROOT/scripts/run_sanitize_gate.sh"
STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-sanitize-gate.XXXXXX")"
FIXTURE="$STATE/project"
FAKE_BIN="$STATE/bin"
LOG="$STATE/commands.log"

cleanup()
{
    rm -rf -- "$STATE"
}
trap cleanup EXIT

mkdir -p "$FIXTURE/bin" "$FAKE_BIN"

cat >"$FAKE_BIN/make" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
printf 'make %s\n' "$*" >>"$PCV_SAN_FAKE_LOG"

if [[ " $* " == *" clean "* ]]; then
    rm -f test_runner bin/purecvisorsd .pcv_fake_instrumented
    exit 0
fi

if [[ "$*" == *"CFLAGS_EXTRA="* ]]; then
    if [[ "${PCV_FAKE_SAN_BUILD_RC:-0}" -ne 0 ]]; then
        exit "$PCV_FAKE_SAN_BUILD_RC"
    fi
    touch .pcv_fake_instrumented
elif [[ " $* " == *" single "* && " $* " == *" test_runner "* ]]; then
    if [[ "${PCV_FAKE_NORMAL_BUILD_RC:-0}" -ne 0 ]]; then
        exit "$PCV_FAKE_NORMAL_BUILD_RC"
    fi
    rm -f .pcv_fake_instrumented
    mkdir -p bin
    printf '#!/usr/bin/env bash\nexit 0\n' >bin/purecvisorsd
    printf '#!/usr/bin/env bash\nexit 0\n' >bin/pcvctl
    chmod +x bin/purecvisorsd bin/pcvctl
else
    exit 97
fi

cat >test_runner <<'RUNNER'
#!/usr/bin/env bash
set -euo pipefail
printf 'runner %s\n' "$*" >>"$PCV_SAN_FAKE_LOG"
if [[ -f .pcv_fake_instrumented ]]; then
    if [[ " $* " != *" -x /privdrop/seccomp_subprocess "* ]]; then
        echo 'LeakSanitizer has encountered a fatal error: does not work under ptrace' >&2
        exit 23
    fi
    echo 'instrumented-suite-pass'
    exit "${PCV_FAKE_SAN_RUN_RC:-0}"
fi
if [[ " $* " == *" -p /privdrop/seccomp_subprocess "* ]]; then
    echo 'normal-seccomp-pass'
    exit "${PCV_FAKE_SECCOMP_RC:-0}"
fi
exit 96
RUNNER
chmod +x test_runner
SH
chmod +x "$FAKE_BIN/make"

cat >"$FAKE_BIN/sudo" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
while [[ "${1:-}" == -* ]]; do shift; done
exec "$@"
SH
chmod +x "$FAKE_BIN/sudo"

cat >"$FAKE_BIN/ldd" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
if [[ -f .pcv_fake_instrumented ]]; then
    echo 'libasan.so.8 => /fake/libasan.so.8'
else
    echo 'libc.so.6 => /fake/libc.so.6'
fi
SH
chmod +x "$FAKE_BIN/ldd"

export PATH="$FAKE_BIN:$PATH"
export PCV_SAN_FAKE_LOG="$LOG"

                                                          
(
    cd "$FIXTURE"
    make CFLAGS_EXTRA=-fsanitize=address LDFLAGS_EXTRA=-fsanitize=address test_runner
)
set +e
RAW_OUTPUT="$(cd "$FIXTURE" && ./test_runner -v 2>&1)"
RAW_RC=$?
set -e
[[ "$RAW_RC" -eq 23 ]]
grep -q 'LeakSanitizer.*ptrace' <<<"$RAW_OUTPUT"
echo 'PASS: unsplit instrumented seccomp fixture reproduces LSan/ptrace failure'

[[ -x "$GATE" ]] || {
    echo "FAIL: missing executable sanitizer gate: $GATE" >&2
    exit 1
}

run_gate()
{
    local san_rc="$1"
    local seccomp_rc="$2"
    local expected_rc="$3"
    : >"$LOG"
    set +e
    PCV_FAKE_SAN_RUN_RC="$san_rc" \
    PCV_FAKE_SECCOMP_RC="$seccomp_rc" \
    PCV_SANITIZE_ROOT="$FIXTURE" \
        "$GATE" >/dev/null 2>&1
    local actual_rc=$?
    set -e
    if [[ "$expected_rc" -eq 0 ]]; then
        [[ "$actual_rc" -eq 0 ]]
    else
        [[ "$actual_rc" -ne 0 ]]
    fi
    grep -q 'runner .* -x /privdrop/seccomp_subprocess' "$LOG"
    grep -q 'runner -p /privdrop/seccomp_subprocess' "$LOG"
    [[ -x "$FIXTURE/test_runner" ]]
    [[ -x "$FIXTURE/bin/purecvisorsd" ]]
    [[ ! -e "$FIXTURE/.pcv_fake_instrumented" ]]
    ! ldd "$FIXTURE/test_runner" | grep -Eq 'libasan|libubsan'
}

run_gate 0 0 0
echo 'PASS: split sanitizer and normal seccomp phases both succeed'

run_gate 41 0 1
echo 'PASS: instrumented-suite failure is not hidden and normal artifacts are restored'

run_gate 0 42 1
echo 'PASS: normal seccomp failure is not hidden and normal artifacts are restored'

echo 'PASS: sanitizer gate acceptance complete'
