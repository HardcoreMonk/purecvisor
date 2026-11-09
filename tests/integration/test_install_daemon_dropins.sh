#!/usr/bin/env bash
                                                                                          
                                                                       
                                                                                      
                                                                       
                                                                           
set -euo pipefail

                                                             
                                                                      
                                                                    
 
                                                       
                                                                   

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HELPER="$ROOT_DIR/scripts/install-daemon-dropins.sh"
SRC="$ROOT_DIR/packaging/systemd"
STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-daemon-dropins.XXXXXX")"
trap 'rm -rf "$STATE"' EXIT
fail() { printf 'FAIL: %s\n' "$1" >&2; exit 1; }

DROPIN_DIR_REL="etc/systemd/system/purecvisorsd.service.d"
SERVICE_REL="etc/systemd/system/proc-hidepid.service"
DROPINS=(95-coredump-hardening.conf 96-hidepid.conf 97-sandbox.conf)

                                                           
ROOT="$STATE/root1"
bash "$HELPER" --src "$SRC" --root "$ROOT" >/dev/null || fail "install failed"
for name in "${DROPINS[@]}"; do
    target="$ROOT/$DROPIN_DIR_REL/$name"
    [[ -f "$target" ]] || fail "drop-in not installed at fixed target: $name"
    [[ "$(stat -c %a "$target")" == "644" ]] || fail "drop-in mode != 644: $name"
    cmp -s "$SRC/$name" "$target" || fail "drop-in content differs from packaging source: $name"
done
service_target="$ROOT/$SERVICE_REL"
[[ -f "$service_target" ]] || fail "proc-hidepid.service not installed at fixed target"
[[ "$(stat -c %a "$service_target")" == "644" ]] || fail "proc-hidepid.service mode != 644"
cmp -s "$SRC/proc-hidepid.service" "$service_target" || fail "proc-hidepid.service content differs"

                                                              
ROOT="$STATE/root2"
mkdir -p "$ROOT/$DROPIN_DIR_REL"
printf '[Unit]\nRequiresMountsFor=/data\n' >"$ROOT/$DROPIN_DIR_REL/95-data-storage.conf"
HOST_SHA="$(sha256sum "$ROOT/$DROPIN_DIR_REL/95-data-storage.conf" | awk '{print $1}')"
printf '# stale sandbox drop-in that must be overwritten\n' >"$ROOT/$DROPIN_DIR_REL/97-sandbox.conf"
bash "$HELPER" --src "$SRC" --root "$ROOT" >/dev/null || fail "install over existing dir failed"
[[ -f "$ROOT/$DROPIN_DIR_REL/95-data-storage.conf" ]] || fail "host-specific drop-in was removed"
[[ "$(sha256sum "$ROOT/$DROPIN_DIR_REL/95-data-storage.conf" | awk '{print $1}')" == "$HOST_SHA" ]] \
    || fail "host-specific drop-in was modified"
cmp -s "$SRC/97-sandbox.conf" "$ROOT/$DROPIN_DIR_REL/97-sandbox.conf" \
    || fail "canonical 97-sandbox.conf was not written over the stale copy"

                                                                 
ROOT="$STATE/root3"
PARTIAL="$STATE/partial-src"
mkdir -p "$PARTIAL"
cp "$SRC/95-coredump-hardening.conf" "$SRC/96-hidepid.conf" "$SRC/proc-hidepid.service" "$PARTIAL/"
                         
set +e
bash "$HELPER" --src "$PARTIAL" --root "$ROOT" >/dev/null 2>&1
rc=$?
set -e
[[ "$rc" -eq 2 ]] || fail "missing canonical drop-in did not exit 2 (got $rc)"
[[ ! -e "$ROOT/$DROPIN_DIR_REL/95-coredump-hardening.conf" ]] \
    || fail "partial source installed files before the existence check"

                                         
set +e
bash "$HELPER" --root "$STATE/root4" >/dev/null 2>&1
[[ $? -eq 2 ]] || fail "missing --src did not exit 2"
bash "$HELPER" --src "$STATE/nonexistent-src" --root "$STATE/root4" >/dev/null 2>&1
[[ $? -eq 2 ]] || fail "nonexistent --src did not exit 2"
set -e

                         
ROOT="$STATE/root1"
bash "$HELPER" --src "$SRC" --root "$ROOT" >/dev/null || fail "re-install failed"
cmp -s "$SRC/97-sandbox.conf" "$ROOT/$DROPIN_DIR_REL/97-sandbox.conf" || fail "re-install content differs"

echo "PASS: daemon drop-in install contract"
