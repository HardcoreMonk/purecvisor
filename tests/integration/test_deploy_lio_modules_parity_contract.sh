#!/usr/bin/env bash
                                                                              
                                                                               
                                                                  
                                                                      
                                                                    
                                                                            
                 
set -euo pipefail

                                                                                     
                                                                                   
                                                                           
                                                                      
                                                                          
                                                                            
 
                                                                        
                                                                                
                                                                          
                                                                         

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DEB="$ROOT_DIR/packaging/deb/build-deb.sh"
DEPLOY="$ROOT_DIR/scripts/deploy.sh"
STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-lio-parity.XXXXXX")"
chmod 0700 "$STATE"
trap 'rm -rf "$STATE"' EXIT

fail() { printf 'FAIL: %s\n' "$1" >&2; exit 1; }

run_contract() {
  python3 - "$1" "$2" <<'PY'
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

                                                                  
CANONICAL = {"purecvisor-lio.conf", "purecvisor-vfio.conf"}

                                                                      
                                                                       
                                                                  
MODPROBE_RE = re.compile(
    r"modprobe\s+-a\s+target_core_mod\s+iscsi_target_mod\s+"
    r"target_core_iblock\s+nf_conntrack_bridge"
)
                                                                             
CONFIGFS_ORACLE = "test -d /sys/kernel/config/target"

                                                                                
                                                            
DEPLOY_INSTALL_RE = re.compile(
    r"install\s+-m\s*0?644\s+\S+\s+(?:\\\s*)?/etc/modules-load\.d/([A-Za-z0-9._-]+)"
)

                                                                   
                                                                                                
                                                                      
deb_set = set(re.findall(r'"\$STAGE/etc/modules-load\.d/([^"]+)"', build_deb))
require(
    deb_set == CANONICAL,
    f"deb-side modules-load.d install set != canonical {sorted(CANONICAL)}: {sorted(deb_set)}",
)

                                                               
remote_start = deploy.find("deploy_node()")
local_start = deploy.find('if [[ $NO_LOCAL -eq 0 ]]; then')
require(
    remote_start >= 0 and local_start > remote_start,
    "deploy.sh is missing the deploy_node()/local boundary",
)
regions = {
    "remote": deploy[remote_start:local_start],
    "local": deploy[local_start:],
}

region_sets = {}
for name, region in regions.items():
    region_set = set(DEPLOY_INSTALL_RE.findall(region))
    require(
        region_set == CANONICAL,
        f"deploy.sh {name} region modules-load.d install set != canonical "
        f"{sorted(CANONICAL)}: {sorted(region_set)}",
    )
                                                                  
    require(
        MODPROBE_RE.search(region) is not None,
        f"deploy.sh {name} region does not modprobe -a the four LIO/bridge modules "
        f"(exact -a form required; deb systemd-modules-load 비-오라클과 대비)",
    )
                                             
    require(
        CONFIGFS_ORACLE in region,
        f"deploy.sh {name} region lacks the configfs target oracle ({CONFIGFS_ORACLE})",
    )
    region_sets[name] = region_set

                                                                      
require(
    deb_set == region_sets["remote"] == region_sets["local"],
    "modules-load.d set parity broken: "
    f"deb={sorted(deb_set)} remote={sorted(region_sets['remote'])} "
    f"local={sorted(region_sets['local'])}",
)

print("lio-modules-parity-ok")
PY
}

if ! run_contract "$BUILD_DEB" "$DEPLOY" >"$STATE/current.out" 2>"$STATE/current.err"; then
  cat "$STATE/current.err" >&2
  fail "deb↔deploy LIO modules-load parity contract is incomplete"
fi

                                                             
python3 - "$DEPLOY" "$STATE/deploy-no-lio.sh" <<'PY'
import pathlib
import re
import sys

source = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
mutated, count = re.subn(
    r'\n[ \t]*sudo install -m 0644 "[^"]+" /etc/modules-load\.d/purecvisor-lio\.conf',
    "",
    source,
)
if count != 2:
    raise SystemExit(f"could not build no-lio counterfactual (removed {count}, expected 2)")
pathlib.Path(sys.argv[2]).write_text(mutated, encoding="utf-8")
PY
if run_contract "$BUILD_DEB" "$STATE/deploy-no-lio.sh" >/dev/null 2>&1; then
  fail "counterfactual removing lio.conf install from deploy must be rejected"
fi

                                                                 
python3 - "$DEPLOY" "$STATE/deploy-no-a.sh" <<'PY'
import pathlib
import sys

source = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
mutated = source.replace("modprobe -a ", "modprobe ")
if mutated == source:
    raise SystemExit("could not build modprobe-no-a counterfactual")
pathlib.Path(sys.argv[2]).write_text(mutated, encoding="utf-8")
PY
if run_contract "$BUILD_DEB" "$STATE/deploy-no-a.sh" >/dev/null 2>&1; then
  fail "counterfactual dropping modprobe -a must be rejected"
fi

                                                             
python3 - "$DEPLOY" "$STATE/deploy-local-no-lio.sh" <<'PY'
import pathlib
import re
import sys

source = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
                                                                   
mutated, count = re.subn(
    r'\n[ \t]*sudo install -m 0644 "\$LIO_MODULES_FILE" /etc/modules-load\.d/purecvisor-lio\.conf',
    "",
    source,
    count=1,
)
if count != 1:
    raise SystemExit("could not build local-only-removed counterfactual")
pathlib.Path(sys.argv[2]).write_text(mutated, encoding="utf-8")
PY
if run_contract "$BUILD_DEB" "$STATE/deploy-local-no-lio.sh" >/dev/null 2>&1; then
  fail "counterfactual removing lio from only the local region must be rejected"
fi

echo "PASS: deb↔deploy LIO modules-load parity contract"
