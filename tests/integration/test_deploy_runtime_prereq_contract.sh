#!/usr/bin/env bash
                                                                                                
                                                         
                                                           
set -euo pipefail

                             
 
     
                                                      
                      
     
                                                              
             
          
                                                            
 
                
                                                    
                                            

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEPLOY_SCRIPT="$ROOT_DIR/scripts/deploy.sh"
STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-deploy-prereq.XXXXXX")"
chmod 0700 "$STATE"

cleanup() {
  local stage

  if [[ -d "$STATE/logs" ]]; then
    while IFS= read -r stage; do
      case "$stage" in
        /tmp/pcv_runtime_prereqs.*) rm -rf -- "$stage" ;;
      esac
    done < <(find "$STATE/logs" -type f -name stages.log -exec cat {} +)
  fi
  rm -rf "$STATE"
}
trap cleanup EXIT

fail() {
  printf 'FAIL: %s\n' "$1" >&2
  exit 1
}

run_contract() {
  local candidate="$1"

  python3 - "$candidate" <<'PY'
import pathlib
import re
import sys


class ContractError(Exception):
    pass


def require(condition, message):
    if not condition:
        raise ContractError(message)


def ordered(text, labels):
    positions = []
    for label, pattern in labels:
        match = re.search(pattern, text, re.MULTILINE)
        require(match is not None, f"missing {label}")
        positions.append(match.start())
    require(positions == sorted(positions), "invalid order: " + " -> ".join(
        label for label, _pattern in labels
    ))


path = pathlib.Path(sys.argv[1])
source = path.read_text(encoding="utf-8")
require(
    "PCV_SSH_BIN" in source and "PCV_SCP_BIN" in source,
    "deploy does not expose test-safe SSH/SCP command injection",
)
for literal in (
    "PCV_NGINX_BIND_IP",
    "install-nginx-termination.sh",
    "wait-for-local-ip.sh",
    "--nginx-bind-ip",
    "--rollback",
    "--commit",
    "curl --fail --silent --show-error",
    "--connect-timeout 3 --max-time \"$curl_max\"",
    "--max-filesize 1048576",
    "disabled_by_config",
    "external_termination",
):
    require(literal in source, f"missing nginx transaction deploy contract: {literal}")
require(
    source.count('[[ -n "$NGINX_BIND_IP" ]]') >= 4,
    "nginx mutation must remain explicit opt-in",
)
for literal in (
    "PCV_NGINX_VERIFY_TIMEOUT",
    "timeout --foreground",
    "trap local_nginx_cleanup EXIT",
    "trap rollback_nginx_transaction EXIT",
    "local_nginx_deadline=$((SECONDS + NGINX_VERIFY_TIMEOUT))",
    "nginx_deadline=$((SECONDS + NGINX_VERIFY_TIMEOUT))",
    'mktemp -d "/tmp/pcv_nginx_source.XXXXXXXXXX"',
    'install -m 0700 "$WAIT_FOR_LOCAL_IP_HELPER"',
    'chmod 0700 "$RUNTIME_STAGE/install-nginx-termination.sh"',
    '"$RUNTIME_STAGE/wait-for-local-ip.sh"',
):
    require(literal in source, f"missing bounded rollback contract: {literal}")

build_start = source.find('if [[ $SKIP_BUILD -eq 0 ]]; then')
predeploy_start = source.find('info "=== ZFS Version Check ==="')
require(build_start >= 0 and predeploy_start > build_start, "missing build boundary")
build = source[build_start:predeploy_start]
ordered(
    build,
    (
        ("make release", r"^[ \t]*make release(?:[ \t]|$)"),
        ("make bpf", r"^[ \t]*make bpf(?:[ \t]|$)"),
    ),
)
require(
    re.search(r"fi\s*\n(?:.|\n)*--verify-only(?:.|\n)*--bpf-stage", build)
    is not None,
    "--skip-build path does not validate existing BPF assets",
)
for asset in (
    "build/bpf/pcv_lsm.bpf.o",
    "build/bpf/pcv_shared_bridge.bpf.o",
    "build/bpf/manifest.json",
):
    require(asset in source, f"missing deploy asset path: {asset}")

deploy_start = source.find("deploy_node()")
local_start = source.find('if [[ $NO_LOCAL -eq 0 ]]; then')
require(deploy_start >= 0 and local_start > deploy_start, "missing deploy_node boundary")
remote_deploy = source[deploy_start:local_start]
require(
    re.search(r"mkdir -m 0700 -- [\"']?\$1", remote_deploy)
    is not None,
    "remote runtime stage is not created with mode 0700",
)
require(
    "PCV_RUNTIME_STAGE=" in remote_deploy,
    "remote runtime stage handshake has no fixed marker",
)
require(
    re.search(r"\[0-9a-f\]\{32\}", remote_deploy) is not None,
    "remote runtime stage is not forced to mode 0700",
)

scp_matches = list(
    re.finditer(
        r'(?ms)^[ \t]*(?:if ! )?(?:scp|"\$SCP_BIN")[ \t].*?; then',
        remote_deploy,
    )
)
runtime_scp = next(
    (
        match.group(0)
        for match in scp_matches
        if "$RUNTIME_PREREQ_HELPER" in match.group(0)
    ),
    None,
)
require(runtime_scp is not None, "missing runtime prerequisite scp block")
for asset in (
    "$RUNTIME_PREREQ_HELPER",
    "$BPF_OBJECT",
    "$BPF_SHARED_OBJECT",
    "$BPF_MANIFEST",
    "$HOST_TUNING_HELPER",
    "$HOST_TUNING_UNIT",
):
    require(asset in runtime_scp, f"runtime scp does not stage {asset}")
require(
    re.search(
        r'if \[\[ -n "\$NGINX_BIND_IP" \]\]; then.*?'
        r'"\$NGINX_TERMINATION_HELPER".*?"\$WAIT_FOR_LOCAL_IP_HELPER".*?'
        r'"\$\{SSH_USER\}@\$\{ip\}:\$\{runtime_stage\}/"',
        remote_deploy,
        re.DOTALL,
    )
    is not None,
    "nginx helpers are not conditionally staged together",
)

remote_match = re.search(
    r"<<'REMOTE_EOF'\n(?P<body>.*?)\nREMOTE_EOF",
    remote_deploy,
    re.DOTALL,
)
require(remote_match is not None, "missing quoted remote deployment heredoc")
remote = remote_match.group("body")
require(
    re.search(r"^[ \t]*set -e(?:u|o|[ \t])", remote, re.MULTILINE) is not None,
    "remote deployment does not fail closed with set -e",
)
require(
    re.search(r"^[ \t]*trap [^\n]+ EXIT$", remote, re.MULTILINE) is not None,
    "remote runtime stage has no EXIT cleanup trap",
)
require(
    "trap rollback_nginx_transaction EXIT" in remote,
    "remote deployment has no nginx rollback EXIT trap",
)
ordered(
    remote,
    (
        ("service stop", r"sudo systemctl stop"),
        ("binary copy", r"sudo cp [^\n]*DAEMON_BIN"),
        ("UI copy", r"sudo cp [^\n]*pcv_ui_"),
        (
            "runtime helper",
            r"^[ \t]*sudo [^\n]*install-runtime-prereqs[^\n]*--bpf-stage",
        ),
        ("service start", r"sudo systemctl start"),
    ),
)
helper_line = re.search(
    r"^[ \t]*sudo [^\n]*install-runtime-prereqs[^\n]*--bpf-stage[^\n]*$",
    remote,
    re.MULTILINE,
)
require(helper_line is not None, "missing unconditional remote runtime helper")
require(
    "||" not in helper_line.group(0) and not helper_line.group(0).lstrip().startswith("if "),
    "remote runtime helper failure can be ignored",
)
require(
    re.search(
        r'^[ \t]*sudo "\$RUNTIME_STAGE/install-host-tuning\.sh"',
        remote,
        re.MULTILINE,
    )
    is not None,
    "missing remote host-tuning install call",
)
require(
    re.search(
        r'--unit "\$RUNTIME_STAGE/purecvisor-host-tuning\.service"',
        remote,
    )
    is not None,
    "remote host-tuning install call missing --unit",
)

local = source[local_start:]
ordered(
    local,
    (
        ("local service stop", r"sudo systemctl stop"),
        ("local binary copy", r"sudo cp [^\n]*DAEMON_BIN"),
        (
            "local runtime helper",
            r"sudo [^\n]*(?:install-runtime-prereqs|\$RUNTIME_PREREQ_HELPER)"
            r"[^\n]*--bpf-stage",
        ),
        ("local service start", r"sudo systemctl start"),
    ),
)
require(
    "if [[ $NO_LOCAL -eq 0 ]]" in local,
    "local runtime installation does not honor --no-local",
)
require(
    re.search(
        r'^[ \t]*sudo "\$HOST_TUNING_HELPER"',
        local,
        re.MULTILINE,
    )
    is not None,
    "missing local host-tuning install call",
)
require(
    re.search(
        r'--unit "\$HOST_TUNING_UNIT"',
        local,
    )
    is not None,
    "local host-tuning install call missing --unit",
)

print("contract-ok")
PY
}

if ! run_contract "$DEPLOY_SCRIPT" >"$STATE/current.out" 2>"$STATE/current.err"; then
  cat "$STATE/current.err" >&2
  fail "deploy runtime prerequisite contract is incomplete"
fi

                                                         
awk '
  !($0 ~ /sudo/ && $0 ~ /install-runtime-prereqs/ && $0 ~ /--bpf-stage/)
' "$DEPLOY_SCRIPT" >"$STATE/no-helper.sh"
if run_contract "$STATE/no-helper.sh" >/dev/null 2>&1; then
  fail "counterfactual without helper execution must be rejected"
fi

                                                            
python3 - "$DEPLOY_SCRIPT" "$STATE/no-runtime-scp.sh" <<'PY'
import pathlib
import re
import sys

source = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
mutated, count = re.subn(
    r'(?ms)^[ \t]*if ! "\$SCP_BIN"[ \t].*?\$RUNTIME_PREREQ_HELPER.*?; then',
    "",
    source,
    count=1,
)
if count != 1:
    raise SystemExit("could not build missing-stage counterfactual")
pathlib.Path(sys.argv[2]).write_text(mutated, encoding="utf-8")
PY
if run_contract "$STATE/no-runtime-scp.sh" >/dev/null 2>&1; then
  fail "counterfactual without runtime BPF staging must be rejected"
fi

                                                                   
awk '!($0 ~ /^[[:space:]]*make bpf([[:space:]]|$)/)' \
  "$DEPLOY_SCRIPT" >"$STATE/no-make-bpf.sh"
if run_contract "$STATE/no-make-bpf.sh" >/dev/null 2>&1; then
  fail "counterfactual without make bpf must be rejected"
fi

FAKE_BIN="$STATE/fake-bin"
mkdir -p "$FAKE_BIN"

cat >"$FAKE_BIN/ssh" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$*" >>"$FAKE_LOG_DIR/ssh.log"
while [[ "${1:-}" == -* ]]; do
  case "$1" in
    -o) shift 2 ;;
    *) shift ;;
  esac
done
[[ $# -gt 0 ]] || exit 2
shift

if [[ "${1:-}" == *'/sys/module/zfs/version'* ]]; then
  printf 'OK\n'
  exit 0
fi
if [[ "${1:-}" == *'systemctl is-active'* ]]; then
  case "${FAKE_HEALTH_STATUS:-active}" in
    active) printf 'active\n'; exit 0 ;;
    inactive) printf 'inactive\n'; exit 0 ;;
    unknown) exit 1 ;;
    *) exit 2 ;;
  esac
fi
if [[ "${1:-}" == *'mktemp -d /tmp/pcv_runtime_prereqs.'* ]]; then
  stage="$(bash -c "$1")"
  printf '%s\n' "$stage" >>"$FAKE_LOG_DIR/stages.log"
  case "${FAKE_STAGE_RESPONSE:-clean}" in
    noisy) printf 'unexpected-banner\n%s\n' "$stage" ;;
    invalid) printf 'invalid-stage-response\n' ;;
    *) printf '%s\n' "$stage" ;;
  esac
  exit 0
fi
if [[ "${1:-}" == "bash" && "${2:-}" == "-s" ]]; then
  shift 2
  [[ "${1:-}" == "--" ]] && shift
  script="$FAKE_LOG_DIR/remote-script.$$.sh"
  output="$FAKE_LOG_DIR/remote-output.$$.txt"
  cat >"$script"
                                                       
                                             
                                                  
  remote_args=()
  for arg in "$@"; do
    [[ -n "$arg" ]] && remote_args+=("$arg")
  done
  set +e
  PATH="$FAKE_BIN:$PATH" bash "$script" "${remote_args[@]}" >"$output"
  rc=$?
  set -e
  if grep -Fq 'PCV_RUNTIME_STAGE=' "$script"; then
    stage="${1:-}"
    [[ -d "$stage" ]] && printf '%s\n' "$stage" >>"$FAKE_LOG_DIR/stages.log"
    case "${FAKE_STAGE_RESPONSE:-clean}" in
      noisy) printf 'unexpected-banner\n'; cat "$output" ;;
      invalid) printf 'invalid-stage-response\n' ;;
      *) cat "$output" ;;
    esac
  else
    cat "$output"
  fi
  rm -f "$script" "$output"
  exit "$rc"
fi
exit 2
SH

cat >"$FAKE_BIN/scp" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$*" >>"$FAKE_LOG_DIR/scp.log"
args=()
while (( $# > 0 )); do
  case "$1" in
    -o) shift 2 ;;
    -r) shift ;;
    *) args+=("$1"); shift ;;
  esac
done
(( ${#args[@]} >= 2 )) || exit 2
destination="${args[${#args[@]} - 1]}"
remote_path="${destination#*:}"
if [[ "$remote_path" == /tmp/pcv_runtime_prereqs.*/* ]]; then
  stage="${remote_path%/}"
  printf '%s\n' "$(stat -c '%a' "$stage")" >>"$FAKE_LOG_DIR/stage-modes.log"
  for (( index = 0; index < ${#args[@]} - 1; index++ )); do
    cp "${args[$index]}" "$stage/"
  done
fi
SH

cat >"$FAKE_BIN/sudo" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$*" >>"$FAKE_LOG_DIR/sudo.log"
if [[ "${1:-}" == "systemctl" ]]; then
  if [[ "${2:-}" == "daemon-reload" && "${FAKE_ROLLBACK_RECOVERY_FAIL:-}" == "reload" ]]; then
    exit 1
  fi
  if [[ "${2:-}" == "restart" && "${3:-}" == "purecvisorsd" &&
        "${FAKE_ROLLBACK_RECOVERY_FAIL:-}" == "service" ]]; then
    exit 1
  fi
  if [[ "${2:-}" == "restart" && "${3:-}" == "nginx" &&
        "${FAKE_ROLLBACK_RECOVERY_FAIL:-}" == "nginx" ]]; then
    exit 1
  fi
  case "${2:-}" in
    start|restart)
      printf '%s %s\n' "${2:-}" "${3:-}" >>"$FAKE_LOG_DIR/start.log"
      ;;
    is-active)
      case "${FAKE_LOCAL_HEALTH_STATUS:-active}" in
        active) printf 'active\n' ;;
        inactive) printf 'inactive\n' ;;
        unknown) exit 1 ;;
        *) exit 2 ;;
      esac
      ;;
  esac
  exit 0
fi
if [[ "${1:-}" == "ss" ]]; then
  printf 'LISTEN 0 128 192.0.2.10:80 0.0.0.0:*\n'
  printf 'LISTEN 0 128 192.0.2.10:443 0.0.0.0:*\n'
  exit 0
fi
if [[ "${1:-}" == "env" ]]; then
  printf 'PCV_NGINX_DEPLOYMENT_ID=%064d\n' 1
  exit 0
fi
if [[ "${1:-}" == */install-nginx-termination.sh ]]; then
  printf '%s\n' "$*" >>"$FAKE_LOG_DIR/nginx-transaction.log"
  if [[ "$*" == *"--rollback"* && "${FAKE_NGINX_ROLLBACK_FAIL:-0}" == "1" ]]; then
    exit 1
  fi
  exit 0
fi
if [[ "${1:-}" == */install-runtime-prereqs.sh ]]; then
  [[ "${FAKE_HELPER_FAIL:-0}" == "0" ]]
  exit $?
fi
exit 0
SH
chmod 0755 "$FAKE_BIN/ssh" "$FAKE_BIN/scp" "$FAKE_BIN/sudo"

cat >"$FAKE_BIN/curl" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$*" >>"$FAKE_LOG_DIR/curl.log"
case "${FAKE_NGINX_HEALTH:-success}" in
  nonresponse) exit 28 ;;
  mismatch)
    printf '%s\n' '{"checks":{"tls":{"enabled":true,"degraded":false,"status":"ok","mode":"internal"}}}'
    ;;
  *)
    printf '%s\n' '{"checks":{"tls":{"enabled":false,"degraded":false,"status":"disabled_by_config","mode":"external_termination"}}}'
    ;;
esac
SH
chmod 0755 "$FAKE_BIN/curl"

prepare_project() {
  local candidate="$1"
  local label="$2"
  local project="$STATE/projects/$label"
  local object_sha shared_object_sha

  mkdir -p \
    "$project/scripts" \
    "$project/build/bpf" \
    "$project/bin" \
    "$project/ui" \
    "$project/systemd" \
    "$project/packaging/systemd"
  cp "$candidate" "$project/scripts/deploy.sh"
  cp "$ROOT_DIR/scripts/install-runtime-prereqs.sh" \
    "$project/scripts/install-runtime-prereqs.sh"
  cp "$ROOT_DIR/scripts/install-nginx-termination.sh" \
    "$project/scripts/install-nginx-termination.sh"
  cp "$ROOT_DIR/scripts/wait-for-local-ip.sh" \
    "$project/scripts/wait-for-local-ip.sh"
  cp "$ROOT_DIR/scripts/install-host-tuning.sh" \
    "$project/scripts/install-host-tuning.sh"
  cp "$ROOT_DIR/packaging/systemd/purecvisor-host-tuning.service" \
    "$project/packaging/systemd/purecvisor-host-tuning.service"
  chmod 0755 \
    "$project/scripts/deploy.sh" \
    "$project/scripts/install-runtime-prereqs.sh" \
    "$project/scripts/install-nginx-termination.sh" \
    "$project/scripts/wait-for-local-ip.sh" \
    "$project/scripts/install-host-tuning.sh"
  printf 'fixture-daemon\n' >"$project/bin/purecvisorsd"
  printf 'fixture-cli\n' >"$project/bin/pcvctl"
  printf 'fixture-bpf-object\n' >"$project/build/bpf/pcv_lsm.bpf.o"
  printf 'fixture-shared-bpf-object\n' \
    >"$project/build/bpf/pcv_shared_bridge.bpf.o"
  object_sha="$(sha256sum "$project/build/bpf/pcv_lsm.bpf.o" | awk '{print $1}')"
  shared_object_sha="$(sha256sum "$project/build/bpf/pcv_shared_bridge.bpf.o" | awk '{print $1}')"
  printf '[{"name":"pcv_lsm","file":"pcv_lsm.bpf.o","sha256":"%s"},{"name":"pcv_shared_bridge","file":"pcv_shared_bridge.bpf.o","sha256":"%s","loader":"network-tc"}]\n' \
    "$object_sha" "$shared_object_sha" >"$project/build/bpf/manifest.json"
  printf '%s\n' "$project"
}

prepare_logs() {
  local label="$1"
  local log_dir="$STATE/logs/$label"
  mkdir -p "$log_dir"
  : >"$log_dir/ssh.log"
  : >"$log_dir/scp.log"
  : >"$log_dir/sudo.log"
  : >"$log_dir/start.log"
  : >"$log_dir/stages.log"
  : >"$log_dir/stage-modes.log"
  : >"$log_dir/nginx-transaction.log"
  : >"$log_dir/curl.log"
  printf '%s\n' "$log_dir"
}

run_remote_deploy() {
  local project="$1"
  local log_dir="$2"
  local helper_fail="$3"
  local stage_response="${4:-clean}"
  local health_status="${5:-active}"

                                                                       
  (
    cd "$project"
    export PATH="$FAKE_BIN:$PATH"
    export FAKE_BIN FAKE_LOG_DIR="$log_dir" FAKE_HELPER_FAIL="$helper_fail"
    export FAKE_STAGE_RESPONSE="$stage_response"
    export FAKE_HEALTH_STATUS="$health_status"
    export PCV_NODES="192.0.2.10"
    export PCV_SSH_BIN="$FAKE_BIN/ssh" PCV_SCP_BIN="$FAKE_BIN/scp"
    scripts/deploy.sh --skip-build --no-local
  ) >"$log_dir/deploy.out" 2>"$log_dir/deploy.err"
}

remote_nginx_health_failure_contract() {
  local candidate="$1"
  local label="$2"
  local health_mode="$3"
  local project log_dir rc started elapsed

  project="$(prepare_project "$candidate" "$label")"
  log_dir="$(prepare_logs "$label")"
  started="$(date +%s)"
  set +e
  (
    cd "$project"
    export PATH="$FAKE_BIN:$PATH"
    export FAKE_BIN FAKE_LOG_DIR="$log_dir" FAKE_HELPER_FAIL=0
    export FAKE_STAGE_RESPONSE=clean FAKE_HEALTH_STATUS=active
    export FAKE_NGINX_HEALTH="$health_mode"
    export PCV_NODES="192.0.2.10" PCV_NGINX_BIND_IP="192.0.2.10"
    export PCV_NGINX_VERIFY_TIMEOUT=1
    export PCV_SSH_BIN="$FAKE_BIN/ssh" PCV_SCP_BIN="$FAKE_BIN/scp"
    scripts/deploy.sh --skip-build --no-local
  ) >"$log_dir/deploy.out" 2>"$log_dir/deploy.err"
  rc=$?
  set -e
  elapsed=$(( $(date +%s) - started ))
  [[ "$rc" -ne 0 && "$elapsed" -le 4 ]] || return 1
  grep -Fq -- '--rollback' "$log_dir/nginx-transaction.log" || return 1
  grep -Fq -- '--finalize-rollback' "$log_dir/nginx-transaction.log" || return 1
  grep -Fq 'systemctl daemon-reload' "$log_dir/sudo.log" || return 1
  grep -Fq 'systemctl restart purecvisorsd' "$log_dir/sudo.log" || return 1
  grep -Fq 'systemctl restart nginx' "$log_dir/sudo.log"
}

local_nginx_health_failure_contract() {
  local candidate="$1"
  local label="$2"
  local project log_dir rc

  project="$(prepare_project "$candidate" "$label")"
  log_dir="$(prepare_logs "$label")"
  set +e
  (
    cd "$project"
    export PATH="$FAKE_BIN:$PATH"
    export FAKE_BIN FAKE_LOG_DIR="$log_dir" FAKE_HELPER_FAIL=0
    export FAKE_LOCAL_HEALTH_STATUS=active FAKE_NGINX_HEALTH=mismatch
    export PCV_NODES="" PCV_NGINX_BIND_IP="192.0.2.10"
    export PCV_NGINX_VERIFY_TIMEOUT=1
    export PCV_SSH_BIN="$FAKE_BIN/ssh" PCV_SCP_BIN="$FAKE_BIN/scp"
    scripts/deploy.sh --skip-build --nodes local
  ) >"$log_dir/deploy.out" 2>"$log_dir/deploy.err"
  rc=$?
  set -e
  [[ "$rc" -ne 0 ]] || return 1
  grep -Fq -- '--rollback' "$log_dir/nginx-transaction.log" || return 1
  grep -Fq -- '--finalize-rollback' "$log_dir/nginx-transaction.log" || return 1
  grep -Fq 'systemctl daemon-reload' "$log_dir/sudo.log" || return 1
  grep -Fq 'systemctl restart purecvisorsd' "$log_dir/sudo.log" || return 1
  grep -Fq 'systemctl restart nginx' "$log_dir/sudo.log"
}

nginx_rollback_failure_contract() {
  local candidate="$1"
  local label="$2"
  local scope="$3"
  local project log_dir rc

  project="$(prepare_project "$candidate" "$label")"
  log_dir="$(prepare_logs "$label")"
  set +e
  (
    cd "$project"
    export PATH="$FAKE_BIN:$PATH"
    export FAKE_BIN FAKE_LOG_DIR="$log_dir" FAKE_HELPER_FAIL=0
    export FAKE_HEALTH_STATUS=active FAKE_LOCAL_HEALTH_STATUS=active
    export FAKE_NGINX_HEALTH=mismatch FAKE_NGINX_ROLLBACK_FAIL=1
    export PCV_NGINX_BIND_IP="192.0.2.10" PCV_NGINX_VERIFY_TIMEOUT=1
    export PCV_SSH_BIN="$FAKE_BIN/ssh" PCV_SCP_BIN="$FAKE_BIN/scp"
    if [[ "$scope" == remote ]]; then
      export PCV_NODES="192.0.2.10"
      scripts/deploy.sh --skip-build --no-local
    else
      export PCV_NODES=""
      scripts/deploy.sh --skip-build --nodes local
    fi
  ) >"$log_dir/deploy.out" 2>"$log_dir/deploy.err"
  rc=$?
  set -e
  [[ "$rc" -ne 0 ]] || return 1
  grep -Fq 'nginx transaction rollback failed' "$log_dir/deploy.err" || return 1
  ! grep -Fq 'systemctl daemon-reload' "$log_dir/sudo.log" || return 1
  ! grep -Fq 'systemctl restart purecvisorsd' "$log_dir/sudo.log"
}

nginx_rollback_recovery_failure_contract() {
  local candidate="$1"
  local label="$2"
  local scope="$3"
  local failure="$4"
  local project log_dir rc

  project="$(prepare_project "$candidate" "$label")"
  log_dir="$(prepare_logs "$label")"
  set +e
  (
    cd "$project"
    export PATH="$FAKE_BIN:$PATH"
    export FAKE_BIN FAKE_LOG_DIR="$log_dir" FAKE_HELPER_FAIL=0
    export FAKE_HEALTH_STATUS=active FAKE_LOCAL_HEALTH_STATUS=active
    export FAKE_NGINX_HEALTH=mismatch
    export FAKE_ROLLBACK_RECOVERY_FAIL="$failure"
                                                            
                                                              
                                                          
                                                                   
    export PCV_NGINX_BIND_IP="192.0.2.10" PCV_NGINX_VERIFY_TIMEOUT=5
    export PCV_SSH_BIN="$FAKE_BIN/ssh" PCV_SCP_BIN="$FAKE_BIN/scp"
    if [[ "$scope" == remote ]]; then
      export PCV_NODES="192.0.2.10"
      scripts/deploy.sh --skip-build --no-local
    else
      export PCV_NODES=""
      scripts/deploy.sh --skip-build --nodes local
    fi
  ) >"$log_dir/deploy.out" 2>"$log_dir/deploy.err"
  rc=$?
  set -e
  [[ "$rc" -ne 0 ]] || return 1
  grep -Fq -- '--rollback' "$log_dir/nginx-transaction.log" || return 1
  ! grep -Fq -- '--finalize-rollback' "$log_dir/nginx-transaction.log" || return 1
  grep -Fq 'nginx rollback service recovery failed; recovery state preserved' \
    "$log_dir/deploy.err" || return 1
  if [[ "$failure" == reload ]]; then
    [[ "$(grep -Fc 'systemctl restart purecvisorsd' "$log_dir/sudo.log")" -eq 0 ]] ||
      return 1
    [[ "$(grep -Fc 'systemctl restart nginx' "$log_dir/sudo.log")" -eq 1 ]] ||
      return 1
  elif [[ "$failure" == service ]]; then
    [[ "$(grep -Fc 'systemctl restart purecvisorsd' "$log_dir/sudo.log")" -eq 1 ]] ||
      return 1
    [[ "$(grep -Fc 'systemctl restart nginx' "$log_dir/sudo.log")" -eq 1 ]] ||
      return 1
  else
    [[ "$(grep -Fc 'systemctl restart purecvisorsd' "$log_dir/sudo.log")" -eq 1 ]] ||
      return 1
    [[ "$(grep -Fc 'systemctl restart nginx' "$log_dir/sudo.log")" -eq 2 ]] ||
      return 1
  fi
}

remote_health_failure_contract() {
  local candidate="$1"
  local label="$2"
  local health_status="$3"
  local project log_dir rc

  project="$(prepare_project "$candidate" "$label")"
  log_dir="$(prepare_logs "$label")"
  set +e
  run_remote_deploy "$project" "$log_dir" 0 "clean" "$health_status"
  rc=$?
  set -e
  [[ "$rc" -ne 0 ]] || return 1
  [[ "$(wc -l <"$log_dir/start.log")" -eq 1 ]] || return 1
  grep -Fq 'Failed: 1 nodes' "$log_dir/deploy.out" || return 1
  stages_are_cleaned "$log_dir"
}

remote_duplicate_failure_contract() {
  local candidate="$1"
  local label="$2"
  local health_status="$3"
  local project log_dir rc

  project="$(prepare_project "$candidate" "$label")"
  log_dir="$(prepare_logs "$label")"
  set +e
  run_remote_deploy "$project" "$log_dir" 1 "clean" "$health_status"
  rc=$?
  set -e
  [[ "$rc" -ne 0 ]] || return 1
  [[ ! -s "$log_dir/start.log" ]] || return 1
  grep -Fq 'Failed: 1 nodes' "$log_dir/deploy.out" || return 1
  stages_are_cleaned "$log_dir"
}

local_duplicate_failure_contract() {
  local candidate="$1"
  local label="$2"
  local project log_dir rc

  project="$(prepare_project "$candidate" "$label")"
  log_dir="$(prepare_logs "$label")"
  set +e
                                                                       
  (
    cd "$project"
    export PATH="$FAKE_BIN:$PATH"
    export FAKE_BIN FAKE_LOG_DIR="$log_dir"
    export FAKE_LOCAL_HEALTH_STATUS="inactive"
    export PCV_NODES=""
    export PCV_SSH_BIN="$FAKE_BIN/ssh" PCV_SCP_BIN="$FAKE_BIN/scp"
    scripts/deploy.sh --skip-build --nodes local
  ) >"$log_dir/deploy.out" 2>"$log_dir/deploy.err"
  rc=$?
  set -e
  [[ "$rc" -ne 0 ]] || return 1
  grep -Fq 'Failed: 1 nodes' "$log_dir/deploy.out"
}

binary_preflight_contract() {
  local candidate="$1"
  local label="$2"
  local missing="$3"
  local deploy_scope="$4"
  local project log_dir rc expected_error

  project="$(prepare_project "$candidate" "$label")"
  log_dir="$(prepare_logs "$label")"
  rm -f "$project/bin/$missing"
  expected_error="Binary not found: bin/$missing"
  set +e
                                                                       
  (
    cd "$project"
    export PATH="$FAKE_BIN:$PATH"
    export FAKE_BIN FAKE_LOG_DIR="$log_dir"
    export PCV_SSH_BIN="$FAKE_BIN/ssh" PCV_SCP_BIN="$FAKE_BIN/scp"
    if [[ "$deploy_scope" == "remote" ]]; then
      export PCV_NODES="192.0.2.10"
      scripts/deploy.sh --skip-build --no-local
    else
      export PCV_NODES=""
      scripts/deploy.sh --skip-build --nodes local
    fi
  ) >"$log_dir/deploy.out" 2>"$log_dir/deploy.err"
  rc=$?
  set -e
  [[ "$rc" -ne 0 ]] || return 1
  [[ ! -s "$log_dir/ssh.log" ]] || return 1
  [[ ! -s "$log_dir/scp.log" ]] || return 1
  [[ ! -s "$log_dir/sudo.log" ]] || return 1
  [[ ! -s "$log_dir/start.log" ]] || return 1
  grep -Fq "$expected_error" "$log_dir/deploy.out" ||
    grep -Fq "$expected_error" "$log_dir/deploy.err"
}

stage_handshake_failure_contract() {
  local candidate="$1"
  local label="$2"
  local response="$3"
  local project log_dir rc

  project="$(prepare_project "$candidate" "$label")"
  log_dir="$(prepare_logs "$label")"
  set +e
  run_remote_deploy "$project" "$log_dir" 0 "$response"
  rc=$?
  set -e
  [[ "$rc" -ne 0 ]] || return 1
  [[ ! -s "$log_dir/start.log" ]] || return 1
  stages_are_cleaned "$log_dir"
}

stages_are_cleaned() {
  local log_dir="$1"
  local stage

  while IFS= read -r stage; do
    [[ -n "$stage" ]] || continue
    [[ ! -e "$stage" ]] || return 1
  done <"$log_dir/stages.log"
}

remote_success_contract() {
  local candidate="$1"
  local label="$2"
  local project log_dir

  project="$(prepare_project "$candidate" "$label")"
  log_dir="$(prepare_logs "$label")"
  run_remote_deploy "$project" "$log_dir" 0 || return 1
  [[ "$(wc -l <"$log_dir/start.log")" -eq 1 ]] || return 1
  [[ -s "$log_dir/stage-modes.log" ]] || return 1
  [[ "$(sort -u "$log_dir/stage-modes.log")" == "700" ]] || return 1
                                                         
                                                     
  grep -Fq '__PCV_NGINX_BIND_IP_EMPTY__' "$log_dir/ssh.log" || return 1
  ! grep -Fq 'install-nginx-termination.sh' "$log_dir/scp.log" || return 1
  [[ ! -s "$log_dir/nginx-transaction.log" ]] || return 1
  stages_are_cleaned "$log_dir"
}

remote_helper_failure_contract() {
  local candidate="$1"
  local label="$2"
  local project log_dir rc

  project="$(prepare_project "$candidate" "$label")"
  log_dir="$(prepare_logs "$label")"
  set +e
  run_remote_deploy "$project" "$log_dir" 1
  rc=$?
  set -e
  [[ "$rc" -ne 0 ]] || return 1
  [[ ! -s "$log_dir/start.log" ]] || return 1
  stages_are_cleaned "$log_dir"
}

preflight_failure_contract() {
  local candidate="$1"
  local label="$2"
  local damage="$3"
  local project log_dir rc expected_error

  project="$(prepare_project "$candidate" "$label")"
  log_dir="$(prepare_logs "$label")"
  if [[ "$damage" == "missing-object" ]]; then
    rm -f "$project/build/bpf/pcv_lsm.bpf.o"
    expected_error="BPF deploy asset missing: $project/build/bpf/pcv_lsm.bpf.o"
  else
    printf 'tampered-object\n' >"$project/build/bpf/pcv_lsm.bpf.o"
    expected_error='error: BPF SHA-256 verification failed'
  fi
  set +e
                                                                       
  (
    cd "$project"
    export PATH="$FAKE_BIN:$PATH"
    export FAKE_BIN FAKE_LOG_DIR="$log_dir"
    export PCV_NODES="192.0.2.10"
    export PCV_SSH_BIN="$FAKE_BIN/ssh" PCV_SCP_BIN="$FAKE_BIN/scp"
    scripts/deploy.sh --skip-build --no-local
  ) >"$log_dir/deploy.out" 2>"$log_dir/deploy.err"
  rc=$?
  set -e
  [[ "$rc" -ne 0 ]] || return 1
  [[ ! -s "$log_dir/ssh.log" && ! -s "$log_dir/scp.log" ]] || return 1
  grep -Fq "$expected_error" "$log_dir/deploy.out" ||
    grep -Fq "$expected_error" "$log_dir/deploy.err"
}

internal_tls_timeout_preflight_contract() {
  local candidate="$1"
  local label="$2"
  local project log_dir rc

  project="$(prepare_project "$candidate" "$label")"
  log_dir="$(prepare_logs "$label")"
  set +e
  (
    cd "$project"
    export PATH="$FAKE_BIN:$PATH"
    export FAKE_BIN FAKE_LOG_DIR="$log_dir"
    export PCV_NODES="192.0.2.10"
    export PCV_NGINX_VERIFY_TIMEOUT='60; false'
    export PCV_SSH_BIN="$FAKE_BIN/ssh" PCV_SCP_BIN="$FAKE_BIN/scp"
    scripts/deploy.sh --skip-build --no-local
  ) >"$log_dir/deploy.out" 2>"$log_dir/deploy.err"
  rc=$?
  set -e
  [[ "$rc" -eq 2 ]] || return 1
  [[ ! -s "$log_dir/ssh.log" && ! -s "$log_dir/scp.log" ]] || return 1
  grep -Fq 'PCV_NGINX_VERIFY_TIMEOUT must be an integer from 1 to 60' \
    "$log_dir/deploy.out" ||
    grep -Fq 'PCV_NGINX_VERIFY_TIMEOUT must be an integer from 1 to 60' \
      "$log_dir/deploy.err"
}

no_local_contract() {
  local candidate="$1"
  local label="$2"
  local project log_dir

  project="$(prepare_project "$candidate" "$label")"
  log_dir="$(prepare_logs "$label")"
                                                                       
  (
    cd "$project"
    export PATH="$FAKE_BIN:$PATH"
    export FAKE_BIN FAKE_LOG_DIR="$log_dir"
    export PCV_NODES=""
    export PCV_SSH_BIN="$FAKE_BIN/ssh" PCV_SCP_BIN="$FAKE_BIN/scp"
    scripts/deploy.sh --skip-build --no-local --nodes local
  ) >"$log_dir/deploy.out" 2>"$log_dir/deploy.err" || return 1
  [[ ! -s "$log_dir/ssh.log" ]] || return 1
  [[ ! -s "$log_dir/scp.log" ]] || return 1
  [[ ! -s "$log_dir/sudo.log" ]] || return 1
  [[ ! -s "$log_dir/start.log" ]]
}

remote_success_contract "$DEPLOY_SCRIPT" "current-success" ||
  fail "fake remote success must stage mode 700 and clean up"
remote_helper_failure_contract "$DEPLOY_SCRIPT" "current-helper-failure" ||
  fail "helper failure must prevent start, fail deploy, and clean up"
preflight_failure_contract "$DEPLOY_SCRIPT" "current-missing" "missing-object" ||
  fail "missing BPF object must fail before SSH/SCP"
preflight_failure_contract "$DEPLOY_SCRIPT" "current-bad-sha" "bad-sha" ||
  fail "bad BPF SHA must fail before SSH/SCP"
internal_tls_timeout_preflight_contract \
  "$DEPLOY_SCRIPT" "current-internal-tls-bad-timeout" ||
  fail "internal TLS must reject an unsafe remote timeout argument before SSH/SCP"
no_local_contract "$DEPLOY_SCRIPT" "current-no-local" ||
  fail "--no-local must execute no local sudo/helper/systemctl path"
binary_preflight_contract \
  "$DEPLOY_SCRIPT" "current-missing-daemon" "purecvisorsd" "remote" ||
  fail "missing daemon under --skip-build must fail before remote commands"
binary_preflight_contract \
  "$DEPLOY_SCRIPT" "current-missing-cli" "pcvctl" "local" ||
  fail "missing CLI under --skip-build must fail before local sudo/systemctl"
stage_handshake_failure_contract \
  "$DEPLOY_SCRIPT" "current-stage-noisy" "noisy" ||
  fail "noisy stage handshake must fail closed and remove the created stage"
stage_handshake_failure_contract \
  "$DEPLOY_SCRIPT" "current-stage-invalid" "invalid" ||
  fail "invalid stage handshake must fail closed and remove the created stage"
remote_health_failure_contract \
  "$DEPLOY_SCRIPT" "current-health-inactive" "inactive" ||
  fail "inactive remote service after start must fail the deployment"
remote_health_failure_contract \
  "$DEPLOY_SCRIPT" "current-health-unknown" "unknown" ||
  fail "UNKNOWN remote service health after start must fail the deployment"
remote_duplicate_failure_contract \
  "$DEPLOY_SCRIPT" "current-remote-duplicate" "unknown" ||
  fail "remote deploy+health failure must count one failed target"
remote_nginx_health_failure_contract \
  "$DEPLOY_SCRIPT" "current-nginx-nonresponse" "nonresponse" ||
  fail "nginx health nonresponse must timeout and rollback the transaction"
remote_nginx_health_failure_contract \
  "$DEPLOY_SCRIPT" "current-nginx-mismatch" "mismatch" ||
  fail "nginx health contract mismatch must timeout and rollback the transaction"
local_nginx_health_failure_contract \
  "$DEPLOY_SCRIPT" "current-local-nginx-mismatch" ||
  fail "local nginx health failure must trigger the armed rollback trap"
nginx_rollback_failure_contract \
  "$DEPLOY_SCRIPT" "current-remote-rollback-failure" "remote" ||
  fail "remote rollback failure must preserve recovery state and skip restarts"
nginx_rollback_failure_contract \
  "$DEPLOY_SCRIPT" "current-local-rollback-failure" "local" ||
  fail "local rollback failure must preserve recovery state and skip restarts"
for scope in remote local; do
  for failure in reload service nginx; do
    nginx_rollback_recovery_failure_contract \
      "$DEPLOY_SCRIPT" "current-${scope}-rollback-${failure}-failure" \
      "$scope" "$failure" ||
      fail "$scope rollback $failure failure must preserve retryable recovery state"
  done
done
local_duplicate_failure_contract \
  "$DEPLOY_SCRIPT" "current-local-duplicate" ||
  fail "local deploy+health failure must count one failed target"

grep -Fq 'sudo apt install -y strace' "$ROOT_DIR/docs/GUIDE.md" ||
  fail "GUIDE must document strace as a required runtime prerequisite gate dependency"

run_guide_contract() {
  python3 - "$1" <<'PY'
import pathlib
import re
import sys

guide = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


section_match = re.search(
    r"^####\s+배포 런타임 전제와 보존 정책\s*\n"
    r"(?P<body>.*?)(?=^#{1,4} |\Z)",
    guide,
    re.MULTILINE | re.DOTALL,
)
require(
    section_match is not None,
    "GUIDE must define a scoped deploy runtime prerequisite section",
)
section = section_match.group("body")
systemd_match = re.search(
    r"^###\s+2\.5\s+systemd 서비스 설치\s*\n"
    r"(?P<body>.*?)(?=^###\s|\Z)",
    guide,
    re.MULTILINE | re.DOTALL,
)
require(
    systemd_match is not None,
    "GUIDE must retain the scoped systemd installation section",
)
systemd_section = systemd_match.group("body")
daemon_config_match = re.search(
    r"^###\s+2\.6\s+daemon\.conf 설정\s*\n"
    r"(?P<body>.*?)(?=^###\s|\Z)",
    guide,
    re.MULTILINE | re.DOTALL,
)
require(
    daemon_config_match is not None,
    "GUIDE must retain the scoped daemon.conf configuration section",
)
daemon_config = daemon_config_match.group("body")
require(
    re.search(r"^###\s+2\.7\s", daemon_config, re.MULTILINE) is None,
    "GUIDE daemon.conf contract scope must stop before the next peer section",
)
daemon_reference_match = re.search(
    r"^###\s+16\.2\s+전체 설정 키\s*\n"
    r"(?P<body>.*?)(?=^##\s|\Z)",
    guide,
    re.MULTILINE | re.DOTALL,
)
require(
    daemon_reference_match is not None,
    "GUIDE must retain the scoped full configuration reference",
)
daemon_reference = daemon_reference_match.group("body")


def require_semantics(patterns, message, forbidden=()):
    require(
        all(re.search(pattern, section) for pattern in patterns)
        and not any(re.search(pattern, section) for pattern in forbidden),
        message,
    )


def list_items(markdown):
    items = []
    for line in markdown.splitlines():
        if line.startswith("- "):
            items.append(line[2:])
        elif items and (line.startswith("  ") or not line.strip()):
            if line.strip():
                items[-1] += " " + line.strip()
    return items


def require_item_semantics(patterns, message, forbidden=()):
    for item in list_items(section):
        if (
            all(re.search(pattern, item) for pattern in patterns)
            and not any(re.search(pattern, item) for pattern in forbidden)
        ):
            return
    raise ContractError(message)


for literal in (
    "/etc/purecvisor/pki",
    "0700",
    "secrets.token_hex(32)",
    "64",
    "0600",
    "/usr/lib/purecvisor/bpf",
    "0755",
    "0644",
    "make bpf",
    "--verify-only",
    "lsm=bpf",
):
    require(literal in section, f"GUIDE runtime section is missing: {literal}")

require_item_semantics(
    (
        r"\[daemon\]\s+`?jwt_secret`?",
        r"(?:누락|빈 값)",
        r"(?:CSPRNG|secrets\.token_hex\(32\))",
        r"(?:생성|만들어|영속화)",
    ),
    "GUIDE must describe CSPRNG generation for a missing or empty daemon JWT",
    forbidden=(r"(?:생성|영속화)하지|만들지",),
)
require_item_semantics(
    (
        r"(?:비어 있지 않은|명시(?:적|된))",
        r"32바이트 미만",
        r"(?:placeholder|반복 문자|weak)",
        r"(?:fail-closed|실패|거부)",
    ),
    "GUIDE must reject explicit short and obviously weak daemon JWT values in one item",
    forbidden=(
        r"(?:fail-closed로\s+)?거부하지",
        r"(?:실패|거부)하지",
        r"32바이트 미만.{0,120}(?:자동 생성|자동 회전한다)",
    ),
)
require_semantics(
    (r"(?:인증서|cert/key)", r"(?:충분한 길이|32바이트 이상)", r"(?:보존|덮어쓰지)"),
    "GUIDE must preserve existing certificates and sufficient daemon JWT values",
    forbidden=(r"(?:보존|덮어쓰지)하지",),
)
require_semantics(
    (r"(?:manifest|매니페스트)", r"schema", r"SHA-256", r"(?:검증|확인)"),
    "GUIDE must validate BPF manifest schema and SHA-256 before installation",
    forbidden=(r"(?:검증|확인)하지",),
)
require_semantics(
    (r"--verify-only", r"staging", r"(?:만|전용)", r"(?:설치 대상|root)", r"접근하지"),
    "GUIDE must scope verify-only to staging without installation-root access",
)
require_semantics(
    (r"preflight", r"(?:원격|SSH|SCP)", r"(?:전에|앞서)", r"(?:실패|중단)"),
    "GUIDE must describe deploy preflight before remote mutation",
)
require_semantics(
    (r"(?:실패|성공)", r"staging", r"(?:cleanup|정리)"),
    "GUIDE must describe staging cleanup on deploy exit",
)
require_semantics(
    (r"lsm=bpf", r"(?:변경|활성화)", r"재부팅", r"(?:자동으로|자동 수행)", r"(?:않|아니)"),
    "GUIDE must state that deploy does not change kernel lsm or reboot",
)
require(
    re.search(
        r"PCV_SECRET_AUTH_JWT_SECRET.*?\[auth\].*?"
        r"(?:우선|먼저).*?"
        r"\[daemon\].*?jwt_secret.*?(?:fallback|호환)",
        section,
        re.DOTALL,
    )
    is not None,
    "GUIDE must distinguish runtime auth priority from deploy helper daemon JWT scope",
)
require(
    "컴파일 기본값은 `80`" in daemon_config,
    "GUIDE must document rest_port 80 as the compile-time default",
)
require(
    "컴파일 기본값은 `8080`" not in daemon_config,
    "GUIDE must not describe rest_port 8080 as the compile-time default",
)
require(
    re.search(
        r"rest_port\s*=\s*8080.*?"
        r"(?:nginx|리버스 프록시).*?명시적 운영 선택",
        daemon_config,
        re.DOTALL,
    )
    is not None,
    "GUIDE must identify example rest_port 8080 as an explicit nginx deployment choice",
)
require(
    re.search(
        r"^\|\s*`rest_port`\s*\|\s*80\s*\|[^|]*\|"
        r"[^|\n]*컴파일 기본값",
        daemon_reference,
        re.MULTILINE,
    )
    is not None,
    "GUIDE daemon reference must list rest_port default 80",
)
for endpoint in (
    "REST: http://0.0.0.0:<configured-port>/api/v1/",
    "Web:  http://0.0.0.0:<configured-port>/ui/",
    "WS:   ws://0.0.0.0:<configured-port>/api/v1/ws/events",
):
    require(
        endpoint in systemd_section,
        f"GUIDE generic startup banner must use configured port: {endpoint}",
    )
require(
    re.search(r"0\.0\.0\.0:8080/(?:api/v1/|ui/)", systemd_section) is None,
    "GUIDE generic startup banner must not imply a fixed 8080 port",
)
PY
}

run_rest_port_source_contract() {
  python3 - "$1" "$2" "$3" "$4" <<'PY'
import pathlib
import re
import sys

main, rest_server, rest_header, config_header = (
    pathlib.Path(path).read_text(encoding="utf-8") for path in sys.argv[1:]
)


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


default_match = re.search(
    r"^#define\s+PCV_DEFAULT_REST_PORT\s+(\d+)\s*$",
    config_header,
    re.MULTILINE,
)
require(default_match is not None, "REST compile-time default is not declared")
require(default_match.group(1) == "80", "REST compile-time default must be 80")
require(
    re.search(
        r"\bgint\s+rest_port\s*=\s*pcv_config_get_rest_port\s*\(\s*\)\s*;",
        main,
    )
    is not None,
    "startup banner must use pcv_config_get_rest_port()",
)
require(
    re.search(
        r"self->port\s*=\s*\(port\s*>\s*0\)\s*\?\s*port\s*:"
        r"\s*\(guint16\)pcv_config_get_rest_port\s*\(\s*\)\s*;",
        rest_server,
    )
    is not None,
    "REST listener missing-port path must use pcv_config_get_rest_port()",
)
PY
}

if ! run_rest_port_source_contract \
  "$ROOT_DIR/src/main.c" \
  "$ROOT_DIR/src/api/rest_server.c" \
  "$ROOT_DIR/src/api/rest_server.h" \
  "$ROOT_DIR/src/utils/pcv_config.h"; then
  fail "REST listener and startup banner must share the missing-config port default"
fi

if ! run_guide_contract "$ROOT_DIR/docs/GUIDE.md"; then
  fail "GUIDE runtime prerequisite contract is incomplete"
fi

run_nginx_docs_contract() {
  python3 - "$1" "$2" "$3" "$4" "$5" "$6" <<'PY'
import pathlib
import re
import sys

guide, policy, scenarios, handoff, adr, adr_index = (
    pathlib.Path(path).read_text(encoding="utf-8") for path in sys.argv[1:]
)


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


vendor_exec = "/usr/sbin/nginx -t -q -g 'daemon on; master_process on;'"
                                            
                                                          
begin_anchor = "<!-- PCV-NGINX-OPERATIONS:BEGIN -->"
end_anchor = "<!-- PCV-NGINX-OPERATIONS:END -->"
canonical_semantics = (
    "Vendor 명령이 불일치하면 설치기는 변경 전에 실패한다.",
    "이 실패는 기존 상태를 변경하지 않고 보존한다.",
    "Vendor 계약은 자동 동기화하지 않는다.",
    "재시도 전에 installer 계약과 테스트를 갱신하고 재검토한다.",
)


def anchored_contract(document_name, document):
    require(
        document.count(begin_anchor) == 1 and document.count(end_anchor) == 1,
        f"{document_name} must contain one documented nginx operations anchor pair",
    )
    begin = document.index(begin_anchor) + len(begin_anchor)
    end = document.index(end_anchor)
    require(begin < end, f"{document_name} nginx operations anchors are reversed")
    contract = document[begin:end]
    for literal in (
        "PCV_NGINX_BIND_IP",
        "nginx",
        "purecvisorsd",
        "루프백 HTTP",
        "wait-for-local-ip",
        "exit 75",
        "exit 1",
        "RestartPreventExitStatus=1",
        "mode=external_termination",
        "enabled=false",
        "degraded=false",
        "status=disabled_by_config",
        "인증서",
        "키",
        "rollback",
        vendor_exec,
        *canonical_semantics,
    ):
        require(literal in contract, f"{document_name} nginx contract missing: {literal}")
    return contract


guide_contract = anchored_contract("GUIDE", guide)
handoff_contract = anchored_contract("handoff", handoff)
require(
    "PCV_NGINX_BIND_IP=192.0.2.10" in handoff_contract
    and "192.0.2.10" in handoff_contract
    and "ExecStartPre" in handoff_contract,
    "public guide must include the documentation-address opt-in and vendor ExecStartPre check",
)
for document_name, document in (
    ("GUIDE", guide_contract),
    ("handoff", handoff_contract),
):
    require(
        "PCV_NGINX_BIND_IP" in document and vendor_exec in document,
        f"{document_name} missing nginx opt-in context or exact vendor ExecStartPre",
    )
require(
    all(
        literal in scenarios
        for literal in (
            "X-Forwarded-For",
            "X-Forwarded-Proto",
            "127.0.0.1",
            "::1",
            "비루프백 peer",
            "신뢰하지 않는다",
        )
    ),
    "service scenarios must verify proxy headers are trusted only from loopback peers",
)
require(
    re.search(r"^상태:\s+Verified\s*$", adr, re.MULTILINE)
    and "2026-07-27" in adr
    and "개정 이력" in adr
    and "원결정" in adr,
    "ADR-0029 must preserve original context and record the revision as Verified",
)
require(
    re.search(
        r"^\|\s*ADR-0029\s*\|\s*verified\s*\|",
        adr_index,
        re.MULTILINE | re.IGNORECASE,
    ),
    "ADR index must match ADR-0029 Verified lifecycle",
)
for command in (
    "make check-runtime-prereqs",
    "make test_runner",
    "./test_runner -r /rest_transport",
    "python3 scripts/tests/test_proxy_identity.py",
    "bash tests/integration/test_nginx_termination_install.sh",
):
    require(command in policy, f"verification policy missing mandatory command: {command}")
require(
    re.search(r"https://192\.0\.2\.10/api/v1/health", scenarios)
    and "status=ok" in scenarios
    and "mode=external_termination" in scenarios
    and "status=disabled_by_config" in scenarios,
    "service scenarios must verify LAN HTTPS and exact health values",
)
PY
}

if ! run_nginx_docs_contract \
  "$ROOT_DIR/docs/GUIDE.md" \
  "$ROOT_DIR/docs/DEVELOPMENT_VERIFICATION_POLICY.md" \
  "$ROOT_DIR/docs/SERVICE_FUNCTIONAL_TEST_SCENARIOS.md" \
  "$ROOT_DIR/docs/GUIDE.md" \
  "$ROOT_DIR/docs/adr/0029-rest-ws-tls-always-on.md" \
  "$ROOT_DIR/docs/ADR_INDEX.md"; then
  fail "nginx external TLS operations documentation contract is incomplete"
fi

run_nginx_trust_boundary_contract() {
  python3 - "$@" <<'PY'
import pathlib
import re
import sys

guide, policy, scenarios, handoff, adr, adr_index, plan, design = (
    pathlib.Path(path).read_text(encoding="utf-8") for path in sys.argv[1:]
)


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


boundary = "PCV-NGINX-TRUST-BOUNDARY: host-loopback"
counterfactual = "PCV-NGINX-COUNTERFACTUAL: untrusted-local-process"
for name, document in (
    ("GUIDE", guide),
    ("verification policy", policy),
    ("service scenarios", scenarios),
    ("handoff", handoff),
    ("ADR-0029", adr),
    ("plan", plan),
    ("design", design),
):
    require(boundary in document, f"{name} missing stable host-loopback trust boundary")
    require(counterfactual in document, f"{name} missing local-process counterfactual")

require(
    re.search(r"^상태:\s+Verified\s*$", adr, re.MULTILINE)
    and "공개 재현 절차" in adr
    and "docs/GUIDE.md" in adr
    and "2026-07-27" in adr,
    "ADR-0029 must retain the dated verification and public reproduction path",
)
require(
    re.search(r"^\|\s*ADR-0029\s*\|\s*verified\s*\|", adr_index, re.I | re.M)
    and boundary in adr_index
    and counterfactual in adr_index,
    "ADR index must match the verified trust-boundary contract",
)
PY
}

trust_contract_args=(
  "$ROOT_DIR/docs/GUIDE.md"
  "$ROOT_DIR/docs/DEVELOPMENT_VERIFICATION_POLICY.md"
  "$ROOT_DIR/docs/SERVICE_FUNCTIONAL_TEST_SCENARIOS.md"
  "$ROOT_DIR/docs/GUIDE.md"
  "$ROOT_DIR/docs/adr/0029-rest-ws-tls-always-on.md"
  "$ROOT_DIR/docs/ADR_INDEX.md"
  "$ROOT_DIR/docs/adr/0029-rest-ws-tls-always-on.md"
  "$ROOT_DIR/docs/adr/0029-rest-ws-tls-always-on.md"
)
if ! run_nginx_trust_boundary_contract "${trust_contract_args[@]}"; then
  fail "nginx host-loopback trust-boundary documentation contract is incomplete"
fi

trust_counterfactual="$STATE/nginx-trust-counterfactual"
cp "$ROOT_DIR/docs/adr/0029-rest-ws-tls-always-on.md" "$trust_counterfactual"
sed -i 's/PCV-NGINX-COUNTERFACTUAL: untrusted-local-process/PCV-NGINX-COUNTERFACTUAL: trusted-local-process/' \
  "$trust_counterfactual"
trust_mutant_args=("${trust_contract_args[@]}")
trust_mutant_args[4]="$trust_counterfactual"
if run_nginx_trust_boundary_contract "${trust_mutant_args[@]}" >/dev/null 2>&1; then
  fail "removing the untrusted-local-process counterfactual must fail"
fi

nginx_docs_fixture="$STATE/nginx-docs-layout"
mkdir -p "$nginx_docs_fixture"
cp "$ROOT_DIR/docs/GUIDE.md" "$nginx_docs_fixture/GUIDE.md"
cp "$ROOT_DIR/docs/DEVELOPMENT_VERIFICATION_POLICY.md" \
  "$nginx_docs_fixture/DEVELOPMENT_VERIFICATION_POLICY.md"
cp "$ROOT_DIR/docs/SERVICE_FUNCTIONAL_TEST_SCENARIOS.md" \
  "$nginx_docs_fixture/SERVICE_FUNCTIONAL_TEST_SCENARIOS.md"
cp "$ROOT_DIR/docs/GUIDE.md" \
  "$nginx_docs_fixture/handoff.md"
cp "$ROOT_DIR/docs/adr/0029-rest-ws-tls-always-on.md" \
  "$nginx_docs_fixture/adr-0029.md"
cp "$ROOT_DIR/docs/ADR_INDEX.md" "$nginx_docs_fixture/ADR_INDEX.md"
python3 - \
  "$nginx_docs_fixture/GUIDE.md" \
  "$nginx_docs_fixture/handoff.md" \
  "$nginx_docs_fixture/ADR_INDEX.md" <<'PY'
import pathlib
import re
import sys


def change_heading_and_insert_prose(path_name, replacement):
    path = pathlib.Path(path_name)
    lines = path.read_text(encoding="utf-8").splitlines()
    context = next(
        index for index, line in enumerate(lines) if "PCV_NGINX_BIND_IP" in line
    )
    heading = next(
        index
        for index in range(context - 1, -1, -1)
        if lines[index].startswith("#") or lines[index].startswith("**")
    )
    lines[heading] = replacement
    lines.insert(context, "무해한 운영 배경 설명을 제목과 계약 사이에 추가한다.")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


change_heading_and_insert_prose(sys.argv[1], "#### fixture nginx 운영 제목")
change_heading_and_insert_prose(sys.argv[2], "### 9.7 fixture nginx 운영 제목")
index_path = pathlib.Path(sys.argv[3])
index = index_path.read_text(encoding="utf-8")
index, count = re.subn(
    r"(^> \*\*현행화 기준:\*\* )\S+$",
    r"\g<1>2099-01-01",
    index,
    count=1,
    flags=re.MULTILINE,
)
if count != 1:
    raise SystemExit("cannot construct ADR index refresh-date fixture")
index_path.write_text(index, encoding="utf-8")
PY
if ! run_nginx_docs_contract \
  "$nginx_docs_fixture/GUIDE.md" \
  "$nginx_docs_fixture/DEVELOPMENT_VERIFICATION_POLICY.md" \
  "$nginx_docs_fixture/SERVICE_FUNCTIONAL_TEST_SCENARIOS.md" \
  "$nginx_docs_fixture/handoff.md" \
  "$nginx_docs_fixture/adr-0029.md" \
  "$nginx_docs_fixture/ADR_INDEX.md"; then
  fail "harmless markdown heading, numbering, and index refresh-date changes must pass"
fi

nginx_docs_command_mutant="$STATE/nginx-docs-command-mutant"
cp -a "$nginx_docs_fixture" "$nginx_docs_command_mutant"
sed -i \
  "s#/usr/sbin/nginx -t -q -g 'daemon on; master_process on;'#/usr/sbin/nginx -t#" \
  "$nginx_docs_command_mutant/GUIDE.md"
if run_nginx_docs_contract \
  "$nginx_docs_command_mutant/GUIDE.md" \
  "$nginx_docs_command_mutant/DEVELOPMENT_VERIFICATION_POLICY.md" \
  "$nginx_docs_command_mutant/SERVICE_FUNCTIONAL_TEST_SCENARIOS.md" \
  "$nginx_docs_command_mutant/handoff.md" \
  "$nginx_docs_command_mutant/adr-0029.md" \
  "$nginx_docs_command_mutant/ADR_INDEX.md" >/dev/null 2>&1; then
  fail "changed vendor ExecStartPre command must fail the docs contract"
fi

assert_nginx_docs_semantic_mutant_fails() {
  local label="$1"
  local original="$2"
  local replacement="$3"
  local mutant="$STATE/nginx-docs-$label-mutant"

  cp -a "$nginx_docs_fixture" "$mutant"
  python3 - "$mutant/handoff.md" "$original" "$replacement" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
document = path.read_text(encoding="utf-8")
updated, count = document.replace(sys.argv[2], sys.argv[3]), document.count(sys.argv[2])
if count != 1:
    raise SystemExit(f"cannot construct semantic mutant: found {count} occurrences")
path.write_text(updated, encoding="utf-8")
PY
  if run_nginx_docs_contract \
    "$mutant/GUIDE.md" \
    "$mutant/DEVELOPMENT_VERIFICATION_POLICY.md" \
    "$mutant/SERVICE_FUNCTIONAL_TEST_SCENARIOS.md" \
    "$mutant/handoff.md" \
    "$mutant/adr-0029.md" \
    "$mutant/ADR_INDEX.md" >/dev/null 2>&1; then
    fail "reversed nginx docs semantic must fail: $label"
  fi
}

assert_nginx_docs_semantic_mutant_fails \
  "fail-before-change" \
  "Vendor 명령이 불일치하면 설치기는 변경 전에 실패한다." \
  "Vendor 명령이 일치하면 설치기는 변경 후에 계속한다."
assert_nginx_docs_semantic_mutant_fails \
  "state-preservation" \
  "이 실패는 기존 상태를 변경하지 않고 보존한다." \
  "이 실패는 기존 상태를 변경하고 폐기한다."
assert_nginx_docs_semantic_mutant_fails \
  "no-auto-sync" \
  "Vendor 계약은 자동 동기화하지 않는다." \
  "Vendor 계약은 자동 동기화한다."
assert_nginx_docs_semantic_mutant_fails \
  "review-before-retry" \
  "재시도 전에 installer 계약과 테스트를 갱신하고 재검토한다." \
  "installer 계약과 테스트를 갱신하거나 재검토하지 않고 재시도한다."

run_gate_registration_contract() {
  python3 - "$1" "$2" <<'PY'
import pathlib
import re
import sys

makefile = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
policy = pathlib.Path(sys.argv[2]).read_text(encoding="utf-8")


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


target = re.search(
    r"^check-runtime-prereqs:\s*\n(?P<body>(?:\t.*\n)+)",
    makefile,
    re.MULTILINE,
)
require(target is not None, "Makefile must define check-runtime-prereqs")
body = target.group("body")
recipe_lines = []
for line in body.splitlines():
    normalized = line.strip()
    if normalized.startswith("@"):
        normalized = normalized[1:].lstrip()
    recipe_lines.append(normalized)
for command in (
    "bash tests/integration/test_runtime_prereq_install.sh",
    "bash tests/integration/test_nginx_termination_install.sh",
    "bash tests/integration/test_deploy_runtime_prereq_contract.sh",
):
    require(
        recipe_lines.count(command) == 1,
        f"check-runtime-prereqs must run exactly one recipe line: {command}",
    )
require(
    "sudo apt install strace" in body,
    "check-runtime-prereqs must retain the strace fail-fast hint",
)

logical_makefile = makefile.replace("\\\n", " ")
phony_targets = {
    target
    for body in re.findall(r"^\.PHONY:(.*)$", logical_makefile, re.MULTILINE)
    for target in body.split()
}
require(
    "check-runtime-prereqs" in phony_targets,
    "check-runtime-prereqs must be .PHONY",
)

check_all = re.search(
    r"(?P<target>^check-all:[^\n]*\n)"
    r"(?P<recipe>(?:\t[^\n]*(?:\n|$))+)",
    makefile,
    re.MULTILINE,
)
require(check_all is not None, "Makefile must define the check-all block")
target_line = check_all.group("target")
recipe = check_all.group("recipe")
deps = target_line.split(":", 1)[1].split()
expected_deps = {
    "check-rbac",
    "check-rpc-consumers",
    "check-dead-exports",
    "check-rpc-param-contract",
    "check-json-ingress",
    "check-safety-controls",
    "check-error-codes",
                                                      
                                                                 
    "check-cli-exit-status",
                                                                              
    "check-network-mode-contract",
                                                               
    "check-iscsi-chap-argv",
    "check-audit-placement",
    "check-cors-anchor",
    "check-secret-logging",
    "check-ssrf-guard",
    "check-grpc-authz",
    "check-ssrf-target-guard",
    "check-audit-hashchain",
    "check-rng-safe",
    "check-uds-authz",
    "check-transport-bind",
    "check-proxy-identity",
    "check-container-owner-scope",
    "check-mtls-wiring",
    "check-tls-min-version",
    "check-secret-wipe",
    "check-security-headers",
    "check-password-policy",
    "check-ws-token-url",
    "check-zpool-suspend-recover",
    "check-deb-apparmor",
    "check-public-comments",
                                                                  
    "check-vendor-integrity",
    "check-npm-lockfile",
    "check-deb-supply-chain",
                                                    
    "check-fe-rpc-params",
    "check-rpc-route-unique",
                                                                      
    "check-rerror-guard",
    "check-runtime-prereqs",
}
require(
    len(deps) == len(set(deps)),
    "check-all dependencies must be unique",
)
require(
    set(deps) == expected_deps,
    "check-all dependencies must match the complete expected 38-gate set",
)
require(
    "전체 통과 (38게이트:" in recipe and "network mode enum" in recipe and
    "iSCSI CHAP argv 제거" in recipe and "runtime prerequisites" in recipe,
    "check-all success message must describe all 38 gates",
)

policy_section = re.search(
    r"^###\s+\d+\.\d+\s+런타임 전제 배포 게이트\s*\n"
    r"(?P<body>.*?)(?=^#{1,3} |\Z)",
    policy,
    re.MULTILINE | re.DOTALL,
)
require(
    policy_section is not None,
    "verification policy must define a runtime prerequisite deployment gate section",
)
section = policy_section.group("body")
trigger = section.split("```bash", 1)[0]
for script in ("scripts/install-runtime-prereqs.sh", "scripts/deploy.sh"):
    require(
        script in trigger,
        f"runtime prerequisite policy trigger must include {script}",
    )
require(
    "바꾸면" in trigger,
    "runtime prerequisite policy must connect runtime/deploy changes to its gate",
)
require(
    re.search(
        r"```bash\s*\nmake check-runtime-prereqs\s*\n```",
        section,
    )
    is not None,
    "runtime prerequisite policy must link the exact make target",
)

bullets = []
for line in section.splitlines():
    if line.startswith("- "):
        bullets.append(line[2:])
    elif bullets and line.startswith("  "):
        bullets[-1] += " " + line.strip()


def require_bullet(patterns, message, forbidden=()):
    require(
        any(
            all(re.search(pattern, bullet) for pattern in patterns)
            and not any(re.search(pattern, bullet) for pattern in forbidden)
            for bullet in bullets
        ),
        message,
    )


require_bullet(
    (r"\bPKI\b", r"\bJWT\b", r"(?:보존|유지)(?:한다|해야)"),
    "runtime prerequisite policy must preserve existing PKI and JWT assets",
    forbidden=(r"(?:보존|유지)하지",),
)
require_bullet(
    (r"\bBPF\b", r"SHA-256", r"(?:검증|확인)(?:하고|한다|해야)"),
    "runtime prerequisite policy must verify BPF SHA-256 integrity",
    forbidden=(r"(?:검증|확인)하지",),
)
require_bullet(
    (r"--verify-only", r"파일시스템", r"(?:접근하지|접근 없이|열지)"),
    "runtime prerequisite policy must keep verify-only away from the install root",
)
require_bullet(
    (r"원격 staging", r"0700", r"(?:cleanup|정리)", r"시작하지"),
    "runtime prerequisite policy must secure and clean remote staging before start",
)
PY
}

if ! run_gate_registration_contract \
  "$ROOT_DIR/Makefile" "$ROOT_DIR/docs/DEVELOPMENT_VERIFICATION_POLICY.md"; then
  fail "runtime prerequisite gate registration contract is incomplete"
fi

python3 - \
  "$ROOT_DIR/Makefile" \
  "$ROOT_DIR/docs/DEVELOPMENT_VERIFICATION_POLICY.md" \
  "$ROOT_DIR/docs/GUIDE.md" \
  "$STATE" <<'PY'
import pathlib
import sys

makefile = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
policy = pathlib.Path(sys.argv[2]).read_text(encoding="utf-8")
guide = pathlib.Path(sys.argv[3]).read_text(encoding="utf-8")
state = pathlib.Path(sys.argv[4])


def write_mutant(name, source, old, new):
    mutated = source.replace(old, new, 1)
    if mutated == source:
        raise SystemExit(f"cannot construct mutant: {name}")
    (state / name).write_text(mutated, encoding="utf-8")


success_line = next(
    line
    for line in makefile.splitlines()
    if "계약 게이트 전체 통과" in line
)
mutated_success = success_line.replace(" + runtime prerequisites)", ")")
if mutated_success == success_line:
    raise SystemExit("cannot construct check-all success message mutant")
write_mutant(
    "check-all-message-missing.mk",
    makefile,
    success_line,
    mutated_success,
)
write_mutant(
    "runtime-recipe-echo.mk",
    makefile,
    "\t@bash tests/integration/test_runtime_prereq_install.sh",
    '\t@echo "bash tests/integration/test_runtime_prereq_install.sh"',
)
write_mutant(
    "runtime-recipe-comment.mk",
    makefile,
    "\t@bash tests/integration/test_deploy_runtime_prereq_contract.sh",
    "\t@# bash tests/integration/test_deploy_runtime_prereq_contract.sh",
)

                                                         
                                                                  
                                                                          
write_mutant(
    "check-all-duplicate.mk",
    makefile,
    " check-runtime-prereqs\n",
    " check-rbac check-runtime-prereqs\n",
)
write_mutant(
    "policy-korean-cleanup.md",
    policy,
    "helper와 BPF 자산을 정리한다.",
    "helper와 BPF 자산을 안전하게 정리한다.",
)
write_mutant(
    "policy-renumbered.md",
    policy,
    "### 4.19 런타임 전제 배포 게이트",
    "### 4.20 런타임 전제 배포 게이트",
)
write_mutant(
    "policy-no-link.md",
    policy,
    "make check-runtime-prereqs",
    "make check-rbac",
)
write_mutant(
    "policy-positive-access.md",
    policy,
    "설치 대상 파일시스템에 접근하지",
    "설치 대상 파일시스템에 접근하고",
)
write_mutant(
    "policy-negated-preserve.md",
    policy,
    "내용은 재배포에서도 보존한다.",
    "내용은 재배포에서도 보존하지 않는다.",
)
write_mutant(
    "policy-negated-verify.md",
    policy,
    "각 객체의 `BPF SHA-256`을 설치 전에 검증하고,",
    "각 객체의 `BPF SHA-256`을 설치 전에 검증하지 않고,",
)
write_mutant(
    "guide-negated-bpf-verify.md",
    guide,
    "schema와 SHA-256을 검증하고",
    "schema와 SHA-256을 검증하지 않고",
)
write_mutant(
    "guide-negated-jwt-generation.md",
    guide,
    "64자리 소문자 hex를 만들어",
    "64자리 소문자 hex를 만들지 않고",
)
write_mutant(
    "guide-wrong-jwt-section.md",
    guide,
    "배포 helper가 관리하는 값은 `[daemon] jwt_secret`이다.",
    "배포 helper가 관리하는 값은 `[auth] jwt_secret`이다.",
)
write_mutant(
    "guide-rest-default-8080.md",
    guide,
    "컴파일 기본값은 `80`",
    "컴파일 기본값은 `8080`",
)
write_mutant(
    "guide-banner-fixed-8080.md",
    guide,
    "0.0.0.0:<configured-port>",
    "0.0.0.0:8080",
)
scope_leak = guide.replace(
    "컴파일 기본값은 `80`",
    "컴파일 기본값은 설정 파일과 별도로 정의된다",
    1,
)
peer_heading = "### 2.7 CLI 자동완성 설치\n"
if scope_leak.count(peer_heading) != 1:
    raise SystemExit("cannot construct GUIDE daemon scope leak mutant")
scope_leak = scope_leak.replace(
    peer_heading,
    peer_heading + "\nREST `rest_port`의 컴파일 기본값은 `80`이다.\n",
    1,
)
(state / "guide-daemon-scope-leak.md").write_text(scope_leak, encoding="utf-8")
PY

if run_gate_registration_contract \
  "$STATE/check-all-message-missing.mk" \
  "$ROOT_DIR/docs/DEVELOPMENT_VERIFICATION_POLICY.md" >/dev/null 2>&1; then
  fail "check-all success message without runtime prerequisites must be rejected"
fi
if run_gate_registration_contract \
  "$STATE/runtime-recipe-echo.mk" \
  "$ROOT_DIR/docs/DEVELOPMENT_VERIFICATION_POLICY.md" >/dev/null 2>&1; then
  fail "echoed runtime prerequisite integration command must be rejected"
fi
if run_gate_registration_contract \
  "$STATE/runtime-recipe-comment.mk" \
  "$ROOT_DIR/docs/DEVELOPMENT_VERIFICATION_POLICY.md" >/dev/null 2>&1; then
  fail "commented runtime prerequisite integration command must be rejected"
fi
if run_gate_registration_contract \
  "$STATE/check-all-duplicate.mk" \
  "$ROOT_DIR/docs/DEVELOPMENT_VERIFICATION_POLICY.md" >/dev/null 2>&1; then
  fail "check-all dependency duplicate must be rejected"
fi
if ! run_gate_registration_contract \
  "$ROOT_DIR/Makefile" \
  "$STATE/policy-korean-cleanup.md" >/dev/null 2>&1; then
  fail "semantically equivalent Korean cleanup wording must be accepted"
fi
if ! run_gate_registration_contract \
  "$ROOT_DIR/Makefile" \
  "$STATE/policy-renumbered.md" >/dev/null 2>&1; then
  fail "runtime prerequisite policy gate must not depend on an exact section number"
fi
if run_gate_registration_contract \
  "$ROOT_DIR/Makefile" \
  "$STATE/policy-no-link.md" >/dev/null 2>&1; then
  fail "verification policy runtime gate without make target link must be rejected"
fi
if run_gate_registration_contract \
  "$ROOT_DIR/Makefile" \
  "$STATE/policy-positive-access.md" >/dev/null 2>&1; then
  fail "verification policy runtime gate without no-access semantics must be rejected"
fi
if run_gate_registration_contract \
  "$ROOT_DIR/Makefile" \
  "$STATE/policy-negated-preserve.md" >/dev/null 2>&1; then
  fail "verification policy must reject negated PKI/JWT preservation"
fi
if run_gate_registration_contract \
  "$ROOT_DIR/Makefile" \
  "$STATE/policy-negated-verify.md" >/dev/null 2>&1; then
  fail "verification policy must reject negated BPF SHA verification"
fi
if run_guide_contract "$STATE/guide-negated-bpf-verify.md" >/dev/null 2>&1; then
  fail "GUIDE must reject negated BPF manifest verification"
fi
if run_guide_contract "$STATE/guide-negated-jwt-generation.md" >/dev/null 2>&1; then
  fail "GUIDE must reject negated missing-JWT generation"
fi
if run_guide_contract "$STATE/guide-wrong-jwt-section.md" >/dev/null 2>&1; then
  fail "GUIDE must reject missing-JWT generation in the wrong section"
fi
if run_guide_contract "$STATE/guide-rest-default-8080.md" >/dev/null 2>&1; then
  fail "GUIDE must reject rest_port 8080 as the compile-time default"
fi
if run_guide_contract "$STATE/guide-banner-fixed-8080.md" >/dev/null 2>&1; then
  fail "GUIDE must reject a fixed 8080 generic startup banner"
fi
if run_guide_contract "$STATE/guide-daemon-scope-leak.md" >/dev/null 2>&1; then
  fail "GUIDE daemon contract must reject semantics leaked into the next peer section"
fi

python3 - "$DEPLOY_SCRIPT" "$STATE" <<'PY'
import pathlib
import sys

source = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
state = pathlib.Path(sys.argv[2])
mutations = {
    "set-plus-e.sh": (
        "        set -euo pipefail",
        "        set +e",
    ),
    "stage-0755.sh": (
        'mkdir -m 0700 -- "$1"',
        'mkdir -m 0755 -- "$1"',
    ),
    "trap-true.sh": (
        "        trap rollback_nginx_transaction EXIT",
        "        trap true EXIT",
    ),
    "guard-inverted.sh": (
        '    if [[ ! -f "$asset" ]]; then',
        '    if [[ -f "$asset" ]]; then',
    ),
    "verify-ignored.sh": (
        '"$RUNTIME_PREREQ_HELPER" --verify-only --bpf-stage "$BPF_STAGE"',
        '"$RUNTIME_PREREQ_HELPER" --verify-only --bpf-stage "$BPF_STAGE" || true',
    ),
    "no-local-comment.sh": (
        "if [[ $NO_LOCAL -eq 0 ]]; then",
        "if true; then # if [[ $NO_LOCAL -eq 0 ]]",
    ),
    "health-failure-ignored.sh": (
        '        if [[ ${NODE_FAILED[i]:-0} -eq 0 ]]; then\n'
        '            FAIL_COUNT=$((FAIL_COUNT + 1))\n'
        '            NODE_FAILED[i]=1\n'
        '        fi',
        '        : # health failure ignored',
    ),
}
for name, (old, new) in mutations.items():
    if source.count(old) < 1:
        raise SystemExit(f"cannot construct mutant: {name}")
    (state / name).write_text(source.replace(old, new, 1), encoding="utf-8")
PY

if remote_helper_failure_contract "$STATE/set-plus-e.sh" "mutant-set-plus-e"; then
  fail "set +e mutant must violate helper failure/start contract"
fi
if remote_success_contract "$STATE/stage-0755.sh" "mutant-stage-0755"; then
  fail "mode 0755 staging mutant must violate mode 0700 contract"
fi
if run_contract "$STATE/trap-true.sh" >/dev/null 2>&1; then
  fail "no-op EXIT trap mutant must violate cleanup contract"
fi
if preflight_failure_contract \
  "$STATE/guard-inverted.sh" "mutant-guard-inverted" "missing-object"; then
  fail "inverted missing-asset guard mutant must violate exact guard contract"
fi
if preflight_failure_contract \
  "$STATE/verify-ignored.sh" "mutant-verify-ignored" "bad-sha"; then
  fail "ignored verify-only failure mutant must contact no remote"
fi
if no_local_contract "$STATE/no-local-comment.sh" "mutant-no-local-comment"; then
  fail "comment-preserved no-local guard mutant must execute no local path"
fi
if remote_health_failure_contract \
  "$STATE/health-failure-ignored.sh" "mutant-health-ignored" "inactive"; then
  fail "health failure accounting mutant must not return deployment success"
fi

printf 'PASS: deploy runtime prerequisite contract\n'
