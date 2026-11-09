#!/usr/bin/env bash
                                                                                              
                                                                    
                                                     
                                                                     
                                                          
set -euo pipefail

                                                         
                                                                           
                                                                      
                                                               
                                               
 
                                                                     
                                    
                                                                   
                                                                

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DEB="$ROOT_DIR/packaging/deb/build-deb.sh"
DEPLOY="$ROOT_DIR/scripts/deploy.sh"
HELPER="$ROOT_DIR/scripts/install-daemon-dropins.sh"
STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-dropin-parity.XXXXXX")"
chmod 0700 "$STATE"
trap 'rm -rf "$STATE"' EXIT

fail() { printf 'FAIL: %s\n' "$1" >&2; exit 1; }

run_contract() {
  python3 - "$1" "$2" "$3" <<'PY'
import pathlib
import re
import sys


class ContractError(Exception):
    pass


def require(condition, message):
    if not condition:
        raise ContractError(message)


build_deb = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
deploy = pathlib.Path(sys.argv[2]).read_text(encoding="utf-8")
helper = pathlib.Path(sys.argv[3]).read_text(encoding="utf-8")

CANONICAL = {
    "95-coredump-hardening.conf",
    "96-hidepid.conf",
    "97-sandbox.conf",
    "proc-hidepid.service",
}

                                                                
                                                                           
                                                                              
deb_dropins = set(
    re.findall(
        r'install\s+-m644\s+\S+\s+"\$STAGE/etc/systemd/system/purecvisorsd\.service\.d/([^"]+)"',
        build_deb,
    )
)
deb_set = set(deb_dropins)
if re.search(
    r'install\s+-m644\s+\S+\s+"\$STAGE/etc/systemd/system/proc-hidepid\.service"',
    build_deb,
):
    deb_set.add("proc-hidepid.service")
                                                                     
require(
    "purecvisor-host-tuning.service" not in deb_set
    and "purecvisor-lxc@.service" not in deb_set,
    "deb-side extraction leaked host-tuning/lxc@ into the drop-in set",
)
require(
    deb_set == CANONICAL,
    f"deb-side drop-in set != canonical four: {sorted(deb_set)}",
)

                                                                
require(
    'DROPIN_DIR="$ROOT/etc/systemd/system/purecvisorsd.service.d"' in helper,
    "helper does not target purecvisorsd.service.d for drop-ins",
)
require(
    'SERVICE_TARGET="$ROOT/etc/systemd/system/proc-hidepid.service"' in helper,
    "helper does not target /etc/systemd/system/proc-hidepid.service",
)
dropins_match = re.search(r"DROPINS=\(([^)]*)\)", helper)
require(dropins_match is not None, "helper has no DROPINS array")
helper_dropins = set(dropins_match.group(1).split())
helper_set = set(helper_dropins)
                                                    
require(
    re.search(
        r'install\s+-D\s+-m644\b[^\n]*"\$SRC/proc-hidepid\.service"\s+"\$SERVICE_TARGET"',
        helper,
    )
    is not None,
    "helper does not install proc-hidepid.service to the fixed target at mode 0644",
)
helper_set.add("proc-hidepid.service")
                                                                  
require(
    re.search(
        r'install\s+-D\s+-m644\b[^\n]*"\$SRC/\$name"\s+"\$DROPIN_DIR/\$name"',
        helper,
    )
    is not None,
    "helper does not install service.d drop-ins at mode 0644 to the fixed dir",
)
require(
    helper_set == CANONICAL,
    f"helper drop-in set != canonical four: {sorted(helper_set)}",
)

                                                                       
require(
    deb_set == helper_set,
    f"deb-side and deploy-side drop-in sets differ: "
    f"deb-only={sorted(deb_set - helper_set)} deploy-only={sorted(helper_set - deb_set)}",
)

                                                               
remote_start = deploy.find("deploy_node()")
local_start = deploy.find('if [[ $NO_LOCAL -eq 0 ]]; then')
require(
    remote_start >= 0 and local_start > remote_start,
    "deploy.sh is missing the deploy_node()/local boundary",
)
remote_region = deploy[remote_start:local_start]
local_region = deploy[local_start:]
helper_call_re = re.compile(
    r'sudo\s+"(?:[^"]*install-daemon-dropins\.sh|\$DAEMON_DROPIN_HELPER)"\s+--src\b'
)
require(
    helper_call_re.search(remote_region) is not None,
    "deploy.sh remote region does not invoke install-daemon-dropins helper",
)
require(
    helper_call_re.search(local_region) is not None,
    "deploy.sh local region does not invoke install-daemon-dropins helper",
)
                                                      
for region_name, region in (("remote", remote_region), ("local", local_region)):
    call_line = next(
        line
        for line in region.splitlines()
        if helper_call_re.search(line)
    )
    require(
        "||" not in call_line and not call_line.lstrip().startswith("if "),
        f"deploy.sh {region_name} helper call must fail closed (no || / if guard)",
    )

                                                                            
require(
    "systemctl enable proc-hidepid.service || true" in helper,
    "helper does not enable proc-hidepid.service best-effort under ROOT=/",
)
require(
    re.search(r"systemctl\s+start\b", helper) is None
    and re.search(r"systemctl\s+restart\b", helper) is None,
    "helper must not live-start proc-hidepid (enable only, next boot)",
)
require(
    re.search(r"mount\b[^\n]*remount", helper) is None,
    "helper must not live-remount /proc (deb-equivalent: next boot only)",
)

                                                               
for name, source in (
    ("helper", helper),
    ("deploy remote", remote_region),
    ("deploy local", local_region),
):
    require(
        re.search(r"rm\s+-rf?\b[^\n]*service\.d", source) is None,
        f"{name} must not delete the service.d directory (non-destructive)",
    )
    require(
        re.search(r"rm\s+-rf?\b[^\n]*service\.d/\*", source) is None,
        f"{name} must not glob-delete service.d contents (preserve host-specific drop-ins)",
    )

print("dropin-parity-ok")
PY
}

if ! run_contract "$BUILD_DEB" "$DEPLOY" "$HELPER" >"$STATE/current.out" 2>"$STATE/current.err"; then
  cat "$STATE/current.err" >&2
  fail "deb↔deploy drop-in parity contract is incomplete"
fi

                                                         
sed 's/96-hidepid\.conf //' "$HELPER" >"$STATE/helper-drop-one.sh"
if cmp -s "$HELPER" "$STATE/helper-drop-one.sh"; then
  fail "could not construct helper drop-one counterfactual"
fi
if run_contract "$BUILD_DEB" "$DEPLOY" "$STATE/helper-drop-one.sh" >/dev/null 2>&1; then
  fail "counterfactual dropping one drop-in from the helper must be rejected"
fi

                                                             
python3 - "$DEPLOY" "$STATE/deploy-remote-only.sh" <<'PY'
import pathlib
import re
import sys

source = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
                                                    
mutated, count = re.subn(
    r'\n[ \t]*sudo "\$DAEMON_DROPIN_HELPER" --src "\$DAEMON_DROPIN_DIR"',
    "",
    source,
    count=1,
)
if count != 1:
    raise SystemExit("could not build local-only-removed counterfactual")
pathlib.Path(sys.argv[2]).write_text(mutated, encoding="utf-8")
PY
if run_contract "$BUILD_DEB" "$STATE/deploy-remote-only.sh" "$HELPER" >/dev/null 2>&1; then
  fail "counterfactual invoking the helper in only one region must be rejected"
fi

                                                             
python3 - "$DEPLOY" "$STATE/deploy-local-only.sh" <<'PY'
import pathlib
import re
import sys

source = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
mutated, count = re.subn(
    r'\n[ \t]*sudo "\$RUNTIME_STAGE/install-daemon-dropins\.sh" --src "\$RUNTIME_STAGE"',
    "",
    source,
    count=1,
)
if count != 1:
    raise SystemExit("could not build remote-only-removed counterfactual")
pathlib.Path(sys.argv[2]).write_text(mutated, encoding="utf-8")
PY
if run_contract "$BUILD_DEB" "$STATE/deploy-local-only.sh" "$HELPER" >/dev/null 2>&1; then
  fail "counterfactual invoking the helper in only the remote region must be rejected"
fi

                                                                   
sed 's#systemctl enable proc-hidepid.service || true#systemctl start proc-hidepid.service#' \
  "$HELPER" >"$STATE/helper-live-start.sh"
if cmp -s "$HELPER" "$STATE/helper-live-start.sh"; then
  fail "could not construct helper live-start counterfactual"
fi
if run_contract "$BUILD_DEB" "$DEPLOY" "$STATE/helper-live-start.sh" >/dev/null 2>&1; then
  fail "counterfactual live-starting proc-hidepid must be rejected"
fi

echo "PASS: deb↔deploy drop-in parity contract"
