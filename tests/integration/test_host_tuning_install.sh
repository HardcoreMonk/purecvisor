#!/usr/bin/env bash
                                                                                       
                                                                 
                                                                     
set -euo pipefail

                                                              
                                                                  
 
                                                       
                                                                

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HELPER="$ROOT_DIR/scripts/install-host-tuning.sh"
UNIT="$ROOT_DIR/packaging/systemd/purecvisor-host-tuning.service"
STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-host-tuning.XXXXXX")"
trap 'rm -rf "$STATE"' EXIT
fail() { printf 'FAIL: %s\n' "$1" >&2; exit 1; }

                                          
bash "$HELPER" --unit "$UNIT" --root "$STATE" >/dev/null || fail "install failed"
TARGET="$STATE/etc/systemd/system/purecvisor-host-tuning.service"
[[ -f "$TARGET" ]] || fail "unit not installed at fixed target path"
[[ "$(stat -c %a "$TARGET")" == "644" ]] || fail "unit mode != 644"
cmp -s "$UNIT" "$TARGET" || fail "unit content differs from packaging source"

                        
bash "$HELPER" --unit "$UNIT" --root "$STATE" >/dev/null || fail "re-install failed"
cmp -s "$UNIT" "$TARGET" || fail "re-install content differs"

                                       
set +e
bash "$HELPER" --root "$STATE" >/dev/null 2>&1
[[ $? -eq 2 ]] || fail "missing --unit did not exit 2"
bash "$HELPER" --unit "$STATE/nonexistent" --root "$STATE" >/dev/null 2>&1
[[ $? -eq 2 ]] || fail "nonexistent --unit did not exit 2"
set -e

echo "PASS: host-tuning install contract"
