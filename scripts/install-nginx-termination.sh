#!/usr/bin/env bash
                          
                                                                          
                                                                  
                                                                                                                             
 
                      
                                                                             

                                                                                  
 
     
                                                              
                                                                                  
                                                                   
                                                                                            
                                                                         
                                                             
                                                               
                          
 
                         
                                                            
                                                   
                                                           
                                                             
                                                                   
 
     
                                                                             
                                                     
 
            
                                  
                         
                                                     
                                             
                                                                
                                                   
                                                        
                                                                    
                 
                                                                     
 
                                                      
                                                          
                                                            
                                                          
                                                
                                                        
                                                                               
                   

set -euo pipefail
umask 077

root=/
action=install
bind_ip=
deployment_id=
action_count=0

usage() {
  printf 'Usage: %s [--root PATH] --nginx-bind-ip ADDRESS\n' "$0" >&2
  printf '       %s [--root PATH] --rollback|--finalize-rollback|--commit DEPLOYMENT_ID\n' "$0" >&2
  exit 2
}

while (($#)); do
  case "$1" in
    --root) [[ $# -ge 2 ]] || usage; root="$2"; shift 2 ;;
    --nginx-bind-ip)
      [[ $# -ge 2 ]] || usage
      action_count=$((action_count + 1)); bind_ip="$2"; shift 2
      ;;
    --rollback)
      [[ $# -ge 2 ]] || usage
      action_count=$((action_count + 1)); action=rollback; deployment_id="$2"; shift 2
      ;;
    --finalize-rollback)
      [[ $# -ge 2 ]] || usage
      action_count=$((action_count + 1)); action=finalize-rollback; deployment_id="$2"; shift 2
      ;;
    --commit)
      [[ $# -ge 2 ]] || usage
      action_count=$((action_count + 1)); action=commit; deployment_id="$2"; shift 2
      ;;
    *) usage ;;
  esac
done

[[ "$action_count" -eq 1 ]] || usage
if [[ "$action" == install ]]; then
  [[ -n "$bind_ip" && -z "$deployment_id" ]] || usage
else
  [[ -z "$bind_ip" && "$deployment_id" =~ ^[0-9a-f]{64}$ ]] || usage
fi

exec python3 - "$root" "$action" "$bind_ip" "$deployment_id" \
  "${PCV_WAIT_FOR_LOCAL_IP_SOURCE:-$(cd "$(dirname "$0")" && pwd)/wait-for-local-ip.sh}" <<'PY'
from __future__ import annotations

import fcntl
import ipaddress
import json
import os
import re
import secrets
import shutil
import stat
import subprocess
import sys
import time


class InstallError(Exception):
    pass


root_arg, action, bind_ip, deployment_id, helper_source = sys.argv[1:]
root_path = os.path.abspath(root_arg)
fixture = root_path != "/"
expected_uid = os.geteuid() if fixture else 0
O_DIRECTORY = os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW
O_REGULAR = os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW
STATE_PARTS = ("var", "lib", "purecvisor", "nginx-termination-transactions")
TARGETS = (
    ("daemon", ("etc", "purecvisor"), "daemon.conf", 0o600),
    ("helper", ("usr", "local", "libexec", "purecvisor"), "wait-for-local-ip", 0o755),
    (
        "dropin",
        ("etc", "systemd", "system", "nginx.service.d"),
        "50-purecvisor-bind-ready.conf",
        0o644,
    ),
)


def fail(message: str) -> None:
    raise InstallError(message)


def safe_component(value: str) -> None:
    if not value or value in (".", "..") or "/" in value or "\0" in value:
        fail("unsafe path component")


def open_dir(parent_fd: int, name: str, create: bool = False, mode: int = 0o755) -> int:
    safe_component(name)
    try:
        return os.open(name, O_DIRECTORY, dir_fd=parent_fd)
    except FileNotFoundError:
        if not create:
            raise
        os.mkdir(name, mode, dir_fd=parent_fd)
        os.fsync(parent_fd)
        return os.open(name, O_DIRECTORY, dir_fd=parent_fd)


def walk(root_fd: int, parts: tuple[str, ...], create: bool = False, mode: int = 0o755) -> int:
    current = os.dup(root_fd)
    try:
        for part in parts:
            next_fd = open_dir(current, part, create, mode)
            os.close(current)
            current = next_fd
        return current
    except BaseException:
        os.close(current)
        raise


def validate_owner_mode(info: os.stat_result, mode: int, label: str) -> None:
    if info.st_uid != expected_uid or stat.S_IMODE(info.st_mode) != mode:
        fail(f"unsafe {label} ownership or mode")


def read_at(
    directory_fd: int,
    name: str,
    *,
    required: bool = False,
    with_stat: bool = False,
) -> bytes | tuple[bytes, os.stat_result] | None:
    safe_component(name)
    try:
        fd = os.open(name, O_REGULAR, dir_fd=directory_fd)
    except FileNotFoundError:
        if required:
            fail(f"required file missing: {name}")
        return None
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode):
            fail(f"unsafe non-regular file: {name}")
        chunks = []
        while True:
            chunk = os.read(fd, 65536)
            if not chunk:
                data = b"".join(chunks)
                return (data, info) if with_stat else data
            chunks.append(chunk)
            if sum(map(len, chunks)) > 1024 * 1024:
                fail(f"file too large: {name}")
    finally:
        os.close(fd)


def write_atomic(
    directory_fd: int,
    name: str,
    data: bytes,
    mode: int,
    owner_uid: int | None = None,
    owner_gid: int | None = None,
) -> None:
    safe_component(name)
    temporary = f".{name}.pcv-{secrets.token_hex(16)}"
    fd = -1
    try:
        fd = os.open(
            temporary,
            os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC | os.O_NOFOLLOW,
            mode,
            dir_fd=directory_fd,
        )
        offset = 0
        while offset < len(data):
            written = os.write(fd, data[offset:])
            if written <= 0:
                fail("short write while preparing atomic file")
            offset += written
        if owner_uid is not None and owner_gid is not None:
            identity_log = (
                os.environ.get("PCV_NGINX_TEST_FCHOWN_LOG") if fixture else None
            )
            if identity_log:
                with open(identity_log, "a", encoding="ascii") as stream:
                    stream.write(f"{owner_uid}:{owner_gid}\n")
            else:
                os.fchown(fd, owner_uid, owner_gid)
        os.fchmod(fd, mode)
        os.fsync(fd)
        os.close(fd)
        fd = -1
        os.rename(temporary, name, src_dir_fd=directory_fd, dst_dir_fd=directory_fd)
        os.fsync(directory_fd)
    finally:
        if fd >= 0:
            os.close(fd)
        try:
            os.unlink(temporary, dir_fd=directory_fd)
        except FileNotFoundError:
            pass


def wait_test_hook(hold_name: str, ready_name: str) -> None:
    """fixture 전용 동기화 지점. 운영 환경에서는 관련 변수가 없어 즉시 반환한다."""
    hold = os.environ.get(hold_name)
    ready = os.environ.get(ready_name)
    if not hold:
        return
    if ready:
        ready_fd = os.open(
            ready,
            os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC | os.O_NOFOLLOW,
            0o600,
        )
        os.close(ready_fd)
    deadline = time.monotonic() + 10
    while os.path.exists(hold):
        if time.monotonic() >= deadline:
            fail("test synchronization timeout")
        time.sleep(0.01)


def crash_point(point: str) -> None:
    if not fixture or os.environ.get("PCV_NGINX_TEST_CRASH_POINT") != point:
        return
    wait_test_hook(
        "PCV_NGINX_TEST_CRASH_HOLD_FILE",
        "PCV_NGINX_TEST_CRASH_READY_FILE",
    )


def error_point(point: str) -> None:
    if fixture and os.environ.get("PCV_NGINX_TEST_ERROR_POINT") == point:
        fail(f"injected transaction error at {point}")


def read_helper_source(path: str) -> bytes:
    """검증한 directory FD와 source FD만 사용해 helper 바이트를 고정한다."""
    effective_uid = os.geteuid()
    sudo_uid_value = os.environ.get("SUDO_UID")
    if fixture and "PCV_NGINX_TEST_EFFECTIVE_UID" in os.environ:
        effective_uid = int(os.environ["PCV_NGINX_TEST_EFFECTIVE_UID"])
        sudo_uid_value = os.environ.get("PCV_NGINX_TEST_SUDO_UID")
    trusted_uid = effective_uid
    if effective_uid == 0 and sudo_uid_value is not None:
        if not sudo_uid_value.isdecimal():
            fail("invalid sudo invoking UID")
        trusted_uid = int(sudo_uid_value)
        if trusted_uid < 0 or trusted_uid > 2**31 - 1:
            fail("invalid sudo invoking UID")
    absolute = os.path.abspath(path)
    parent_path, name = os.path.split(absolute)
    safe_component(name)
    parent_fd = os.open(parent_path, O_DIRECTORY)
    source_fd = -1
    try:
        parent_info = os.fstat(parent_fd)
        if (
            parent_info.st_uid != trusted_uid
            or stat.S_IMODE(parent_info.st_mode) != 0o700
        ):
            fail("wait helper staging directory has unsafe owner or mode")
        source_fd = os.open(name, O_REGULAR, dir_fd=parent_fd)
        before = os.fstat(source_fd)
        if not stat.S_ISREG(before.st_mode):
            fail("wait helper source is not a regular file")
        if before.st_uid != trusted_uid:
            fail("wait helper source has unsafe owner")
        source_mode = stat.S_IMODE(before.st_mode)
        if not source_mode & stat.S_IXUSR or source_mode & 0o077:
            fail("wait helper source has unsafe mode")
        chunks = []
        while True:
            chunk = os.read(source_fd, 65536)
            if not chunk:
                break
            chunks.append(chunk)
            if sum(map(len, chunks)) > 1024 * 1024:
                fail("wait helper source is too large")
        wait_test_hook(
            "PCV_NGINX_TEST_SOURCE_HOLD_FILE",
            "PCV_NGINX_TEST_SOURCE_READY_FILE",
        )
        path_info = os.stat(name, dir_fd=parent_fd, follow_symlinks=False)
        after = os.fstat(source_fd)
        if (
            not stat.S_ISREG(path_info.st_mode)
            or (path_info.st_dev, path_info.st_ino) != (before.st_dev, before.st_ino)
            or (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns)
            != (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns)
        ):
            fail("wait helper source changed during validation")
        return b"".join(chunks)
    finally:
        if source_fd >= 0:
            os.close(source_fd)
        os.close(parent_fd)


def unlink_at(directory_fd: int, name: str) -> None:
    try:
        info = os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
    except FileNotFoundError:
        return
    if not stat.S_ISREG(info.st_mode):
        fail(f"refusing to remove unsafe path: {name}")
    os.unlink(name, dir_fd=directory_fd)
    os.fsync(directory_fd)


def update_ini(original: bytes) -> bytes:
    try:
        text = original.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise InstallError("daemon.conf is not UTF-8") from exc
    lines = text.splitlines(keepends=True)

    def set_key(source: list[str], section: str, key: str, value: str) -> list[str]:
        header = f"[{section}]"
        section_start = None
        section_end = len(source)
        for index, line in enumerate(source):
            stripped = line.strip()
            if stripped == header:
                section_start = index
                continue
            if section_start is not None and stripped.startswith("[") and stripped.endswith("]"):
                section_end = index
                break
        if section_start is None:
            if source and not source[-1].endswith("\n"):
                source[-1] += "\n"
            if source and source[-1].strip():
                source.append("\n")
            source.extend([header + "\n", f"{key}={value}\n"])
            return source
        for index in range(section_start + 1, section_end):
            candidate = source[index].lstrip()
            if candidate.startswith(("#", ";")) or "=" not in candidate:
                continue
            if candidate.split("=", 1)[0].strip() == key:
                newline = "\n" if source[index].endswith("\n") else ""
                source[index] = f"{key}={value}{newline}"
                return source
        source.insert(section_end, f"{key}={value}\n")
        return source

    lines = set_key(lines, "tls", "https_enabled", "false")
    lines = set_key(lines, "server", "bind_plaintext", "loopback")
    return "".join(lines).encode("utf-8")


def state_dir(root_fd: int, create: bool = True) -> int:
    result = walk(root_fd, STATE_PARTS, create=create, mode=0o700)
    info = os.fstat(result)
    validate_owner_mode(info, 0o700, "transaction directory")
    return result


def validate_transaction(
    state_fd: int, txid: str, intent: str = "pending"
) -> tuple[int, dict]:
    safe_component(txid)
    if intent not in ("pending", "rolledback"):
        fail("invalid transaction validation intent")
    tx_name = f"{intent}.{txid}"
    tx_fd = open_dir(state_fd, tx_name)
    validate_owner_mode(os.fstat(tx_fd), 0o700, "transaction")
    manifest_info = os.stat("manifest.json", dir_fd=tx_fd, follow_symlinks=False)
    if not stat.S_ISREG(manifest_info.st_mode):
        fail("unsafe transaction manifest")
    validate_owner_mode(manifest_info, 0o600, "transaction manifest")
    raw = read_at(tx_fd, "manifest.json", required=True)
    try:
        manifest = json.loads(raw)
    except (TypeError, json.JSONDecodeError) as exc:
        raise InstallError("invalid transaction manifest") from exc
    if (
        not isinstance(manifest, dict)
        or set(manifest) != {"version", "deployment_id", "targets"}
        or type(manifest["version"]) is not int
        or manifest["version"] != 1
        or not isinstance(manifest["deployment_id"], str)
        or manifest["deployment_id"] != txid
    ):
        fail("invalid transaction manifest schema")
    targets = manifest["targets"]
    if not isinstance(targets, dict) or set(targets) != {item[0] for item in TARGETS}:
        fail("invalid transaction target set")
    for label, _parts, _name, _mode in TARGETS:
        entry = targets[label]
        if not isinstance(entry, dict) or set(entry) != {
            "present", "backup", "mode", "uid", "gid"
        }:
            fail("invalid transaction target entry")
        if not isinstance(entry["present"], bool):
            fail("invalid transaction presence")
        expected_backup = f"{label}.bak" if entry["present"] else None
        if entry["backup"] != expected_backup:
            fail("invalid transaction backup reference")
        if entry["mode"] is not None and (
            type(entry["mode"]) is not int or not 0 <= entry["mode"] <= 0o7777
        ):
            fail("invalid transaction file mode")
        for identity in ("uid", "gid"):
            if entry[identity] is not None and (
                type(entry[identity]) is not int or entry[identity] < 0
            ):
                fail("invalid transaction ownership")
        if entry["present"] != all(
            entry[field] is not None for field in ("mode", "uid", "gid")
        ):
            fail("invalid transaction metadata presence")
        if expected_backup:
            info = os.stat(expected_backup, dir_fd=tx_fd, follow_symlinks=False)
            if not stat.S_ISREG(info.st_mode):
                fail("unsafe transaction backup")
            validate_owner_mode(info, 0o600, "transaction backup")
    return tx_fd, manifest


def cleanup_state_directory(
    state_fd: int, name: str, *, creating: bool = False
) -> None:
    safe_component(name)
    directory_fd = open_dir(state_fd, name)
    validate_owner_mode(os.fstat(directory_fd), 0o700, "transaction cleanup directory")
    try:
        allowed = {"manifest.json", *(f"{label}.bak" for label, *_rest in TARGETS)}
        for entry_name in os.listdir(f"/proc/self/fd/{directory_fd}"):
            temporary_ok = creating and re.fullmatch(
                r"\.(?:manifest\.json|daemon\.bak|helper\.bak|dropin\.bak)"
                r"\.pcv-[0-9a-f]{32}",
                entry_name,
            )
            if entry_name not in allowed and temporary_ok is None:
                fail("unsafe transaction cleanup entry")
            info = os.stat(entry_name, dir_fd=directory_fd, follow_symlinks=False)
            if not stat.S_ISREG(info.st_mode):
                fail("unsafe transaction cleanup file")
            validate_owner_mode(info, 0o600, "transaction cleanup file")
            unlink_at(directory_fd, entry_name)
            crash_point("cleanup-after-unlink")
            error_point("cleanup-after-unlink")
    finally:
        os.close(directory_fd)
    os.rmdir(name, dir_fd=state_fd)
    os.fsync(state_fd)


def tombstone_transaction(state_fd: int, txid: str, intent: str) -> str:
    if intent not in ("committing", "rolledback"):
        fail("invalid transaction cleanup intent")
    pending_name = f"pending.{txid}"
    cleanup_name = f"{intent}.{txid}"
    os.rename(pending_name, cleanup_name, src_dir_fd=state_fd, dst_dir_fd=state_fd)
    os.fsync(state_fd)
    error_point("transaction-tombstone")
    return cleanup_name


def restore(root_fd: int, state_fd: int, txid: str) -> None:
    tx_fd, manifest = validate_transaction(state_fd, txid)
    try:
        for label, parts, name, mode in TARGETS:
            destination_fd = walk(root_fd, parts, create=True)
            try:
                entry = manifest["targets"][label]
                if entry["present"]:
                    backup = read_at(tx_fd, entry["backup"], required=True)
                    write_atomic(
                        destination_fd,
                        name,
                        backup,
                        entry["mode"],
                        entry["uid"],
                        entry["gid"],
                    )
                else:
                    unlink_at(destination_fd, name)
            finally:
                os.close(destination_fd)
        os.close(tx_fd)
        tx_fd = -1
        crash_point("rollback-restored")
        tombstone_transaction(state_fd, txid, "rolledback")
        crash_point("rollback-tombstone")
    finally:
        if tx_fd >= 0:
            os.close(tx_fd)


def reconcile_state(root_fd: int, state_fd: int) -> list[str]:
    path = f"/proc/self/fd/{state_fd}"
    pending = []
    for name in os.listdir(path):
        if name == ".lock":
            continue
        try:
            info = os.stat(name, dir_fd=state_fd, follow_symlinks=False)
        except FileNotFoundError:
            continue
        if not stat.S_ISDIR(info.st_mode):
            fail("unsafe pending transaction entry")
        validate_owner_mode(info, 0o700, "transaction state")
        prefix, separator, txid = name.partition(".")
        if separator != "." or len(txid) != 64 or any(c not in "0123456789abcdef" for c in txid):
            fail("unsafe transaction state name")
        if prefix == "creating":
            cleanup_state_directory(state_fd, name, creating=True)
        elif prefix == "committing":
            cleanup_state_directory(state_fd, name)
        elif prefix == "rolledback":
            fail("rollback recovery requires explicit finalization")
        elif prefix == "pending":
            pending.append(txid)
        else:
            fail("unknown transaction state")
    return sorted(pending)


def vendor_exec(root_fd: int) -> str:
    vendor_fd = walk(root_fd, ("usr", "lib", "systemd", "system"))
    try:
        raw = read_at(vendor_fd, "nginx.service", required=True)
    finally:
        os.close(vendor_fd)
    commands = [
        line.removeprefix("ExecStartPre=")
        for line in raw.decode("utf-8").splitlines()
        if line.startswith("ExecStartPre=") and line != "ExecStartPre="
    ]
    expected = "/usr/sbin/nginx -t -q -g 'daemon on; master_process on;'"
    if commands != [expected]:
        fail("vendor nginx ExecStartPre contract changed")
    return expected


try:
    root_fd = os.open(root_path, O_DIRECTORY)
    state_fd = state_dir(root_fd)
    lock_fd = os.open(
        ".lock",
        os.O_RDWR | os.O_CREAT | os.O_CLOEXEC | os.O_NOFOLLOW,
        0o600,
        dir_fd=state_fd,
    )
    os.fchmod(lock_fd, 0o600)
    validate_owner_mode(os.fstat(lock_fd), 0o600, "transaction lock")
    try:
        fcntl.flock(lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError as exc:
        raise InstallError("another nginx transaction is active") from exc

    completed_action = False
    if action in ("rollback", "finalize-rollback", "commit"):
        state_names = {}
        for intent in ("pending", "committing", "rolledback"):
            name = f"{intent}.{deployment_id}"
            try:
                info = os.stat(name, dir_fd=state_fd, follow_symlinks=False)
            except FileNotFoundError:
                continue
            if not stat.S_ISDIR(info.st_mode):
                fail("unsafe transaction state")
            validate_owner_mode(info, 0o700, "transaction state")
            state_names[intent] = name

        if len(state_names) > 1:
            fail("multiple transaction states conflict; recovery state preserved")
        if "committing" in state_names:
            if action != "commit":
                fail(
                    "transaction outcome conflicts with requested action; "
                    "recovery state preserved"
                )
            cleanup_state_directory(state_fd, state_names["committing"])
            completed_action = True
        elif "rolledback" in state_names:
            validate_fd, _manifest = validate_transaction(
                state_fd, deployment_id, "rolledback"
            )
            os.close(validate_fd)
            if action == "commit":
                fail(
                    "transaction outcome conflicts with requested action; "
                    "recovery state preserved"
                )
            if action == "finalize-rollback":
                cleanup_state_directory(state_fd, state_names["rolledback"])
            completed_action = True
        elif action == "finalize-rollback":
            fail("rollback finalization requires durable rolledback state")

    if completed_action:
        pass
    elif action == "rollback":
        restore(root_fd, state_fd, deployment_id)
    elif action == "commit":
        tx_fd, manifest = validate_transaction(state_fd, deployment_id)
        os.close(tx_fd)
        cleanup_name = tombstone_transaction(
            state_fd, deployment_id, "committing"
        )
        crash_point("commit-tombstone")
        cleanup_state_directory(state_fd, cleanup_name)
    else:
        try:
            parsed = ipaddress.ip_address(bind_ip)
        except ValueError as exc:
            raise InstallError("nginx bind address must be one IPv4 literal") from exc
        if parsed.version != 4 or str(parsed) != bind_ip:
            fail("nginx bind address must be one canonical IPv4 literal")
        pending = reconcile_state(root_fd, state_fd)
        if len(pending) > 1:
            fail("multiple pending nginx transactions require manual recovery")
        if pending:
            restore(root_fd, state_fd, pending[0])
            fail(
                "pending nginx transaction rolled back; "
                "rollback recovery requires explicit finalization"
            )

        helper = read_helper_source(helper_source)
        vendor = vendor_exec(root_fd)
        deployment_id = secrets.token_hex(32)
        creating_name = f"creating.{deployment_id}"
        pending_name = f"pending.{deployment_id}"
        os.mkdir(creating_name, 0o700, dir_fd=state_fd)
        os.fsync(state_fd)
        crash_point("creating-directory")
        tx_fd = open_dir(state_fd, creating_name)
        manifest = {"version": 1, "deployment_id": deployment_id, "targets": {}}
        installed = False
        prepared = False
        try:
            snapshots = {}
            snapshot_modes = {}
            snapshot_uids = {}
            snapshot_gids = {}
            for label, parts, name, _mode in TARGETS:
                destination_fd = walk(root_fd, parts, create=True)
                try:
                    snapshot = read_at(destination_fd, name, with_stat=True)
                    if snapshot is not None:
                        snapshots[label], target_info = snapshot
                        snapshot_modes[label] = stat.S_IMODE(target_info.st_mode)
                        snapshot_uids[label] = target_info.st_uid
                        snapshot_gids[label] = target_info.st_gid
                        if fixture and os.environ.get("PCV_NGINX_TEST_CAPTURE_UID"):
                            snapshot_uids[label] = int(
                                os.environ["PCV_NGINX_TEST_CAPTURE_UID"]
                            )
                            snapshot_gids[label] = int(
                                os.environ["PCV_NGINX_TEST_CAPTURE_GID"]
                            )
                    else:
                        snapshots[label] = None
                        snapshot_modes[label] = None
                        snapshot_uids[label] = None
                        snapshot_gids[label] = None
                finally:
                    os.close(destination_fd)
                present = snapshots[label] is not None
                manifest["targets"][label] = {
                    "present": present,
                    "backup": f"{label}.bak" if present else None,
                    "mode": snapshot_modes[label],
                    "uid": snapshot_uids[label],
                    "gid": snapshot_gids[label],
                }
                if present:
                    write_atomic(tx_fd, f"{label}.bak", snapshots[label], 0o600)
            crash_point("creating-backups")
            write_atomic(
                tx_fd,
                "manifest.json",
                json.dumps(manifest, sort_keys=True, separators=(",", ":")).encode(),
                0o600,
            )
            os.close(tx_fd)
            tx_fd = -1
            os.rename(
                creating_name,
                pending_name,
                src_dir_fd=state_fd,
                dst_dir_fd=state_fd,
            )
            os.fsync(state_fd)
            prepared = True
            crash_point("pending-before-mutation")

            daemon_fd = walk(root_fd, ("etc", "purecvisor"), create=True)
            helper_fd = walk(root_fd, ("usr", "local", "libexec", "purecvisor"), create=True)
            dropin_fd = walk(
                root_fd, ("etc", "systemd", "system", "nginx.service.d"), create=True
            )
            try:
                daemon_data = snapshots["daemon"] or b""
                write_atomic(daemon_fd, "daemon.conf", update_ini(daemon_data), 0o600)
                write_atomic(helper_fd, "wait-for-local-ip", helper, 0o755)
                dropin = (
                    "[Unit]\nStartLimitIntervalSec=0\n\n[Service]\nExecStartPre=\n"
                    f"ExecStartPre=/usr/local/libexec/purecvisor/wait-for-local-ip {bind_ip} 60\n"
                    f"ExecStartPre={vendor}\nRestart=on-failure\n"
                    "RestartPreventExitStatus=1\nRestartSec=5s\n"
                ).encode()
                write_atomic(
                    dropin_fd, "50-purecvisor-bind-ready.conf", dropin, 0o644
                )
                if os.environ.get("PCV_NGINX_INSTALL_FAILPOINT") == "after-write":
                    fail("injected installer failure")
            finally:
                os.close(daemon_fd)
                os.close(helper_fd)
                os.close(dropin_fd)
            installed = True
            wait_test_hook(
                "PCV_NGINX_TEST_HOLD_FILE",
                "PCV_NGINX_TEST_READY_FILE",
            )
        except BaseException:
            if tx_fd >= 0:
                try:
                    os.close(tx_fd)
                except OSError:
                    pass
            if prepared:
                restore(root_fd, state_fd, deployment_id)
            else:
                cleanup_state_directory(state_fd, creating_name, creating=True)
            raise
        finally:
            if installed and tx_fd >= 0:
                os.close(tx_fd)
        reload_command = (
            os.environ.get("PCV_NGINX_TEST_DAEMON_RELOAD_COMMAND")
            if fixture
            else None
        )
        if not fixture or reload_command:
            reload_timeout = 10.0
            command = ["systemctl", "daemon-reload"]
            if fixture:
                reload_timeout = float(
                    os.environ.get("PCV_NGINX_TEST_DAEMON_RELOAD_TIMEOUT", "1")
                )
                if not 0.05 <= reload_timeout <= 2:
                    fail("invalid fixture daemon-reload timeout")
                command = [reload_command]
            try:
                subprocess.run(
                    command,
                    check=True,
                    timeout=reload_timeout,
                )
            except BaseException:
                restore(root_fd, state_fd, deployment_id)
                raise InstallError(
                    "systemd daemon-reload failed; rollback state preserved"
                )
        print(f"PCV_NGINX_DEPLOYMENT_ID={deployment_id}")
except (
    InstallError,
    OSError,
    UnicodeError,
    json.JSONDecodeError,
    subprocess.SubprocessError,
) as exc:
    print(f"error: {exc}", file=sys.stderr)
    raise SystemExit(1)
finally:
    for name in ("lock_fd", "state_fd", "root_fd"):
        fd = globals().get(name)
        if isinstance(fd, int):
            try:
                os.close(fd)
            except OSError:
                pass
PY
