#!/usr/bin/env bash
                                                                                          
                                                          
                                                                   
set -euo pipefail

                      
 
     
                                                        
                          
     
                                                 
                                 
          
                                                         
                           
 
                
                                              
                                             
                            

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-runtime-prereq.XXXXXX")"
trap 'rm -rf "$STATE"' EXIT
chmod 0700 "$STATE"

FAKE_ROOT="$STATE/root"
PRESERVE_ROOT="$STATE/preserve-root"
TRACE_ROOT="$STATE/trace-root"
VERIFY_TARGET="$STATE/verify-target"
VERIFY_ROOT="$STATE/verify-root-symlink"
STAGE="$STATE/stage"
HELPER="$ROOT_DIR/scripts/install-runtime-prereqs.sh"
EXPECTED_REDACTED_CONFIG="$STATE/expected-daemon-redacted.conf"

fail() {
  printf 'FAIL: %s\n' "$1" >&2
  exit 1
}

file_checksum() {
  sha256sum "$1" | awk '{print $1}'
}

tree_checksum() {
  find "$@" -type f -print0 |
    sort -z |
    xargs -0 sha256sum |
    sha256sum |
    awk '{print $1}'
}

tree_file_list() {
  find "$1" -type f -printf '%P\n' | sort
}

tree_mode_list() {
  find "$1" -type f -printf '%m %P\n' | sort
}

tree_fingerprint() {
  python3 - "$1" <<'PY'
import hashlib
import os
import pathlib
import stat
import sys

root = pathlib.Path(sys.argv[1])
records = []
for path in [root, *sorted(root.rglob("*"))]:
    metadata = path.lstat()
    relative = "." if path == root else str(path.relative_to(root))
    digest = "-"
    if stat.S_ISREG(metadata.st_mode):
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
    elif stat.S_ISLNK(metadata.st_mode):
        digest = "link:" + os.readlink(path)
    records.append(
        (
            relative,
            metadata.st_ino,
            stat.S_IMODE(metadata.st_mode),
            metadata.st_uid,
            metadata.st_gid,
            metadata.st_size,
            metadata.st_mtime_ns,
            metadata.st_ctime_ns,
            digest,
        )
    )
for record in records:
    print("\t".join(str(value) for value in record))
PY
}

jwt_value_checksum() {
  awk '
    /^\[[^]]+\][[:space:]]*$/ {
      section = $0
      gsub(/^[[:space:]]*\[|\][[:space:]]*$/, "", section)
      next
    }
    section == "daemon" && /^[[:space:]]*jwt_secret[[:space:]]*=/ {
      sub(/^[^=]*=[[:space:]]*/, "")
      print
      exit
    }
  ' "$1" | sha256sum | awk '{print $1}'
}

run_helper() {
  local root="$1"
  local label="$2"

  if ! "$HELPER" --root "$root" --bpf-stage "$STAGE/bpf" \
    >"$STATE/$label.stdout" 2>"$STATE/$label.stderr"; then
    fail "runtime prerequisite helper must succeed for $label"
  fi
}

mkdir -p "$FAKE_ROOT/etc/purecvisor" "$STAGE/bpf"
cat >"$FAKE_ROOT/etc/purecvisor/daemon.conf" <<'CONF'
                              
[daemon]
rest_port=8080
jwt_secret=

[security_group]
resync_interval_sec=300
CONF
cat >"$EXPECTED_REDACTED_CONFIG" <<'CONF'
                              
[daemon]
rest_port=8080
jwt_secret=<redacted>

[security_group]
resync_interval_sec=300
CONF

printf 'fixture-bpf-object\n' >"$STAGE/bpf/pcv_lsm.bpf.o"
stage_bpf_sha="$(file_checksum "$STAGE/bpf/pcv_lsm.bpf.o")"
printf '[{"name":"pcv_lsm","file":"pcv_lsm.bpf.o","sha256":"%s","requires":["btf","lsm-bpf"]}]\n' \
  "$stage_bpf_sha" >"$STAGE/bpf/manifest.json"

                                                            
                                                    
mkdir -p \
  "$VERIFY_TARGET/etc/purecvisor/pki" \
  "$VERIFY_TARGET/usr/lib/purecvisor/bpf"
cat >"$VERIFY_TARGET/etc/purecvisor/daemon.conf" <<'CONF'
[daemon]
jwt_secret=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
CONF
printf 'operator-cert\n' >"$VERIFY_TARGET/etc/purecvisor/pki/server.crt"
printf 'installed-object\n' >"$VERIFY_TARGET/usr/lib/purecvisor/bpf/old.bpf.o"
printf 'installed-manifest\n' \
  >"$VERIFY_TARGET/usr/lib/purecvisor/bpf/manifest.json"
chmod 0600 "$VERIFY_TARGET/etc/purecvisor/daemon.conf"
chmod 0700 "$VERIFY_TARGET/etc/purecvisor/pki"
ln -s "$VERIFY_TARGET" "$VERIFY_ROOT"
verify_before="$(tree_fingerprint "$VERIFY_TARGET")"

if ! "$HELPER" --verify-only --root "$VERIFY_ROOT" --bpf-stage "$STAGE/bpf" \
  >"$STATE/verify-only.stdout" 2>"$STATE/verify-only.stderr"; then
  fail "verify-only must validate a correct BPF stage without opening the root"
fi
verify_after="$(tree_fingerprint "$VERIFY_TARGET")"
[[ "$verify_before" == "$verify_after" ]] ||
  fail "verify-only must preserve config, PKI, JWT, destination, and metadata"
[[ -L "$VERIFY_ROOT" && "$(readlink "$VERIFY_ROOT")" == "$VERIFY_TARGET" ]] ||
  fail "verify-only must not resolve or replace the root access sentinel"
grep -Fxq 'runtime prerequisites: verified' "$STATE/verify-only.stdout" ||
  fail "verify-only success must emit only the fixed verification status"
[[ ! -s "$STATE/verify-only.stderr" ]] ||
  fail "verify-only success must not emit diagnostics"

verify_only_access_contract() {
  local candidate="$1"
  local label="$2"
  local trace="$STATE/$label.strace"
  local root_token target_token

  root_token="$(basename "$VERIFY_ROOT")"
  target_token="$(basename "$VERIFY_TARGET")"
  command -v strace >/dev/null ||
    fail "strace is required (Ubuntu: sudo apt install strace)"
  if ! strace -f -qq -e trace=%file -o "$trace" \
    "$candidate" --verify-only --root "$VERIFY_ROOT" --bpf-stage "$STAGE/bpf" \
    >"$STATE/$label.stdout" 2>"$STATE/$label.stderr"; then
    return 1
  fi
                                                                
  if grep -F -e "$root_token" -e "$target_token" "$trace" >/dev/null; then
    return 1
  fi
}

verify_only_access_contract "$HELPER" "verify-no-access" ||
  fail "verify-only must issue zero file syscalls against root/config/PKI/JWT/dest"

                                                       
python3 - "$HELPER" "$STATE/verify-root-stat-mutant.sh" <<'PY'
import pathlib
import re
import sys

source = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
pattern = r"(?m)^        if verify_only:\n(?:[ \t]*\n)*            stage_fd = open_stage\(sys\.argv\[2\]\)"
replacement = """        if verify_only:
            os.stat(sys.argv[1])
            stage_fd = open_stage(sys.argv[2])"""
source, count = re.subn(pattern, replacement, source, count=1)
if count != 1:
    raise SystemExit("cannot construct verify-only root-stat mutant")
pathlib.Path(sys.argv[2]).write_text(source, encoding="utf-8")
PY
chmod 0755 "$STATE/verify-root-stat-mutant.sh"
if verify_only_access_contract \
  "$STATE/verify-root-stat-mutant.sh" "verify-root-stat-mutant"; then
  fail "verify-only os.stat(root) mutant must violate the no-access contract"
fi

if [[ ! -x "$HELPER" ]]; then
  "$HELPER" --root "$FAKE_ROOT" --bpf-stage "$STAGE/bpf"
fi
run_helper "$FAKE_ROOT" "first-install"

[[ "$(stat -c '%a' "$STATE")" == "700" ]] ||
  fail "captured helper output must stay below a mode 700 fixture directory"
[[ "$(stat -c '%a' "$FAKE_ROOT/etc/purecvisor/pki")" == "700" ]] ||
  fail "PKI directory mode must be 700 after first install"
[[ "$(stat -c '%a' "$FAKE_ROOT/etc/purecvisor/daemon.conf")" == "600" ]] ||
  fail "daemon.conf mode must be 600 after first install"
[[ "$(stat -c '%a' "$FAKE_ROOT/usr/lib/purecvisor/bpf/pcv_lsm.bpf.o")" == "644" ]] ||
  fail "installed BPF object mode must be 644 after first install"
[[ "$(stat -c '%a' "$FAKE_ROOT/usr/lib/purecvisor/bpf/manifest.json")" == "644" ]] ||
  fail "installed BPF manifest mode must be 644 after first install"
cmp -s "$STAGE/bpf/pcv_lsm.bpf.o" \
  "$FAKE_ROOT/usr/lib/purecvisor/bpf/pcv_lsm.bpf.o" ||
  fail "installed BPF object must match the validated staging object"
cmp -s "$STAGE/bpf/manifest.json" \
  "$FAKE_ROOT/usr/lib/purecvisor/bpf/manifest.json" ||
  fail "installed BPF manifest must match the validated staging manifest"

python3 - \
  "$FAKE_ROOT/etc/purecvisor/daemon.conf" \
  "$STATE/actual-daemon-redacted.conf" <<'PY'
import pathlib
import re
import sys

config = pathlib.Path(sys.argv[1])
redacted_path = pathlib.Path(sys.argv[2])
lines = config.read_text(encoding="utf-8").splitlines(keepends=True)
section = None
daemon_count = 0
other_count = 0
redacted = []

for line in lines:
    section_match = re.match(r"^\s*\[([^]]+)\]\s*(?:\r?\n)?$", line)
    if section_match:
        section = section_match.group(1).strip()
        redacted.append(line)
        continue

    key_match = re.match(
        r"^(\s*jwt_secret\s*=\s*)(.*?)(\r?\n)?$",
        line,
    )
    if not key_match:
        redacted.append(line)
        continue

    if section == "daemon":
        daemon_count += 1
        if not re.fullmatch(r"[0-9a-f]{64}", key_match.group(2).strip()):
            raise SystemExit("daemon JWT must be exactly 64 lowercase hex characters")
    else:
        other_count += 1

    newline = key_match.group(3) or ""
    redacted.append(f"{key_match.group(1)}<redacted>{newline}")

if daemon_count != 1:
    raise SystemExit("daemon section must contain exactly one jwt_secret")
if other_count != 0:
    raise SystemExit("jwt_secret must not appear outside the daemon section")

redacted_path.write_text("".join(redacted), encoding="utf-8")
PY
cmp -s "$EXPECTED_REDACTED_CONFIG" "$STATE/actual-daemon-redacted.conf" ||
  fail "JWT update must preserve comments, rest_port, section order, and other settings"

first_tree_checksum="$(
  tree_checksum \
    "$FAKE_ROOT/etc/purecvisor" \
    "$FAKE_ROOT/usr/lib/purecvisor/bpf"
)"
run_helper "$FAKE_ROOT" "second-install"
second_tree_checksum="$(
  tree_checksum \
    "$FAKE_ROOT/etc/purecvisor" \
    "$FAKE_ROOT/usr/lib/purecvisor/bpf"
)"
[[ "$first_tree_checksum" == "$second_tree_checksum" ]] ||
  fail "a second install must leave all config, PKI, and BPF file checksums unchanged"
[[ "$(stat -c '%a' "$FAKE_ROOT/usr/lib/purecvisor/bpf/pcv_lsm.bpf.o")" == "644" ]] ||
  fail "installed BPF object mode must remain 644 after a repeated install"
[[ "$(stat -c '%a' "$FAKE_ROOT/usr/lib/purecvisor/bpf/manifest.json")" == "644" ]] ||
  fail "installed BPF manifest mode must remain 644 after a repeated install"

mkdir -p \
  "$PRESERVE_ROOT/etc/purecvisor/pki" \
  "$PRESERVE_ROOT/usr/lib/purecvisor/bpf"
{
  printf '%s\n' '# operator config must survive' '[daemon]' 'rest_port=8443'
  printf '%s\n' \
    'jwt_secret=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef'
  printf '%s\n' '' '[security_group]' 'resync_interval_sec=600'
} >"$PRESERVE_ROOT/etc/purecvisor/daemon.conf"
printf 'operator-certificate-fixture\n' \
  >"$PRESERVE_ROOT/etc/purecvisor/pki/server.crt"
printf 'operator-private-key-fixture\n' \
  >"$PRESERVE_ROOT/etc/purecvisor/pki/server.key"
chmod 0755 "$PRESERVE_ROOT/etc/purecvisor/pki"
chmod 0644 "$PRESERVE_ROOT/etc/purecvisor/daemon.conf"

preserved_jwt_checksum="$(
  jwt_value_checksum "$PRESERVE_ROOT/etc/purecvisor/daemon.conf"
)"
preserved_config_checksum="$(
  file_checksum "$PRESERVE_ROOT/etc/purecvisor/daemon.conf"
)"
preserved_cert_checksum="$(
  file_checksum "$PRESERVE_ROOT/etc/purecvisor/pki/server.crt"
)"
preserved_key_checksum="$(
  file_checksum "$PRESERVE_ROOT/etc/purecvisor/pki/server.key"
)"

run_helper "$PRESERVE_ROOT" "preserve-install"

[[ "$(stat -c '%a' "$PRESERVE_ROOT/etc/purecvisor/pki")" == "700" ]] ||
  fail "an existing PKI directory mode must be corrected from 755 to 700"
[[ "$(stat -c '%a' "$PRESERVE_ROOT/etc/purecvisor/daemon.conf")" == "600" ]] ||
  fail "an existing daemon.conf mode must be corrected from 644 to 600"
[[ "$preserved_jwt_checksum" == "$(
  jwt_value_checksum "$PRESERVE_ROOT/etc/purecvisor/daemon.conf"
)" ]] || fail "an existing 64-character-or-longer JWT secret must be preserved"
[[ "$preserved_config_checksum" == "$(
  file_checksum "$PRESERVE_ROOT/etc/purecvisor/daemon.conf"
)" ]] || fail "an operator-managed daemon.conf must remain content-identical"
[[ "$preserved_cert_checksum" == "$(
  file_checksum "$PRESERVE_ROOT/etc/purecvisor/pki/server.crt"
)" ]] || fail "an existing operator certificate must remain checksum-identical"
[[ "$preserved_key_checksum" == "$(
  file_checksum "$PRESERVE_ROOT/etc/purecvisor/pki/server.key"
)" ]] || fail "an existing operator private key must remain checksum-identical"

mkdir -p "$TRACE_ROOT/etc/purecvisor"
cat >"$TRACE_ROOT/etc/purecvisor/daemon.conf" <<'CONF'
                                    
[daemon]
rest_port=9443
jwt_secret=

[security_group]
resync_interval_sec=900
CONF

if ! bash -x "$HELPER" --root "$TRACE_ROOT" --bpf-stage "$STAGE/bpf" \
  >"$STATE/generation-trace.stdout" 2>"$STATE/generation-trace.stderr"; then
  fail "runtime prerequisite helper must generate a JWT successfully under bash trace"
fi

installed_bpf_tree_checksum="$(
  tree_checksum "$FAKE_ROOT/usr/lib/purecvisor/bpf"
)"
installed_bpf_file_list="$(
  tree_file_list "$FAKE_ROOT/usr/lib/purecvisor/bpf"
)"
installed_bpf_mode_list="$(
  tree_mode_list "$FAKE_ROOT/usr/lib/purecvisor/bpf"
)"
printf 'tampered-staging-bpf-object\n' >"$STAGE/bpf/pcv_lsm.bpf.o"

set +e
"$HELPER" --root "$FAKE_ROOT" --bpf-stage "$STAGE/bpf" \
  >"$STATE/sha-mismatch.out" 2>&1
sha_mismatch_rc=$?
set -e

[[ "$sha_mismatch_rc" -ne 0 ]] ||
  fail "a staging BPF SHA mismatch must make the installer fail"
[[ "$installed_bpf_tree_checksum" == "$(
  tree_checksum "$FAKE_ROOT/usr/lib/purecvisor/bpf"
)" ]] || fail "a rejected BPF object must not change installed BPF tree content"
[[ "$installed_bpf_file_list" == "$(
  tree_file_list "$FAKE_ROOT/usr/lib/purecvisor/bpf"
)" ]] || fail "a rejected BPF object must not change the installed BPF file list"
[[ "$installed_bpf_mode_list" == "$(
  tree_mode_list "$FAKE_ROOT/usr/lib/purecvisor/bpf"
)" ]] || fail "a rejected BPF object must not change installed BPF file modes"

printf \
  '[{"name":"pcv_lsm","file":"pcv_lsm.bpf.o\\u0000escape","sha256":"%s"}]\n' \
  "$stage_bpf_sha" >"$STAGE/bpf/manifest.json"

set +e
"$HELPER" --root "$FAKE_ROOT" --bpf-stage "$STAGE/bpf" \
  >"$STATE/nul-manifest.out" 2>&1
nul_manifest_rc=$?
set -e

[[ "$nul_manifest_rc" -ne 0 ]] ||
  fail "a decoded NUL in a BPF manifest filename must make the installer fail"
[[ "$(wc -l <"$STATE/nul-manifest.out")" -eq 1 ]] ||
  fail "a decoded NUL filename failure must emit one safe error line"
grep -Fxq 'error: BPF manifest file value contains NUL' \
  "$STATE/nul-manifest.out" ||
  fail "a decoded NUL filename failure must emit the safe validation status"
if grep -Eq 'Traceback|ValueError' "$STATE/nul-manifest.out"; then
  fail "a decoded NUL filename failure must not expose a Python exception"
fi
[[ "$installed_bpf_tree_checksum" == "$(
  tree_checksum "$FAKE_ROOT/usr/lib/purecvisor/bpf"
)" ]] || fail "a decoded NUL filename must not change installed BPF tree content"
[[ "$installed_bpf_file_list" == "$(
  tree_file_list "$FAKE_ROOT/usr/lib/purecvisor/bpf"
)" ]] || fail "a decoded NUL filename must not change the installed BPF file list"
[[ "$installed_bpf_mode_list" == "$(
  tree_mode_list "$FAKE_ROOT/usr/lib/purecvisor/bpf"
)" ]] || fail "a decoded NUL filename must not change installed BPF file modes"

python3 "$ROOT_DIR/tests/integration/runtime_prereq_security_cases.py" \
  "$HELPER" "$STATE/security"

python3 - \
  "$FAKE_ROOT/etc/purecvisor/daemon.conf" \
  "$PRESERVE_ROOT/etc/purecvisor/daemon.conf" \
  "$TRACE_ROOT/etc/purecvisor/daemon.conf" \
  "$VERIFY_TARGET/etc/purecvisor/daemon.conf" \
  "$STATE/first-install.stdout" \
  "$STATE/first-install.stderr" \
  "$STATE/second-install.stdout" \
  "$STATE/second-install.stderr" \
  "$STATE/preserve-install.stdout" \
  "$STATE/preserve-install.stderr" \
  "$STATE/verify-only.stdout" \
  "$STATE/verify-only.stderr" \
  "$STATE/generation-trace.stdout" \
  "$STATE/generation-trace.stderr" \
  "$STATE/sha-mismatch.out" \
  "$STATE/nul-manifest.out" <<'PY'
import pathlib
import re
import sys

config_paths = [pathlib.Path(value) for value in sys.argv[1:5]]
output_paths = [pathlib.Path(value) for value in sys.argv[5:]]
secrets = []

for config_path in config_paths:
    section = None
    found = []
    for line in config_path.read_text(encoding="utf-8").splitlines():
        section_match = re.match(r"^\s*\[([^]]+)\]\s*$", line)
        if section_match:
            section = section_match.group(1).strip()
            continue
        key_match = re.match(r"^\s*jwt_secret\s*=\s*(.*?)\s*$", line)
        if section == "daemon" and key_match:
            found.append(key_match.group(1))
    if len(found) != 1 or not found[0]:
        raise SystemExit("cannot validate helper output without one daemon JWT")
    secrets.append(found[0])

for output_path in output_paths:
    output = output_path.read_text(encoding="utf-8", errors="replace")
    if any(secret in output for secret in secrets):
        raise SystemExit("helper output or bash trace exposed a JWT secret")
PY

printf 'PASS: runtime prerequisite install contract\n'
