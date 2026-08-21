#!/usr/bin/env bash
                          
                                                                       
                                               
                                                                                                                   
 
                      
                                                                         
set -euo pipefail
umask 077

                                                     
 
     
                                                     
                                                             
                                                                
     
                                                      
     
                                            
 
                 
                                                                
                                                             
                                                               
 
                
                                                  
                                               
                                          

usage() {
  printf 'usage: %s [--verify-only] [--root PATH] --bpf-stage PATH\n' "$0" >&2
}

argument_error() {
  printf 'error: invalid or missing argument\n' >&2
  usage
  exit 2
}

root_argument="/"
bpf_stage_argument=""
verify_only_argument=0
root_seen=0
bpf_stage_seen=0
verify_only_seen=0

while (( $# > 0 )); do
  case "$1" in
    --root)
      (( $# >= 2 )) || argument_error
      (( root_seen == 0 )) || argument_error
      [[ -n "$2" ]] || argument_error
      root_argument="$2"
      root_seen=1
      shift 2
      ;;
    --bpf-stage)
      (( $# >= 2 )) || argument_error
      (( bpf_stage_seen == 0 )) || argument_error
      [[ -n "$2" ]] || argument_error
      bpf_stage_argument="$2"
      bpf_stage_seen=1
      shift 2
      ;;
    --verify-only)
      (( verify_only_seen == 0 )) || argument_error
      verify_only_argument=1
      verify_only_seen=1
      shift
      ;;
    *)
      argument_error
      ;;
  esac
done

(( bpf_stage_seen == 1 )) || argument_error

exec python3 - "$root_argument" "$bpf_stage_argument" "$verify_only_argument" <<'PY'
import errno
import fcntl
import hashlib
import hmac
import json
import os
import re
import secrets
import signal
import stat
import sys
from contextlib import contextmanager
from dataclasses import dataclass


MANIFEST_LIMIT = 1024 * 1024
OBJECT_LIMIT = 256 * 1024 * 1024
JSON_DEPTH_LIMIT = 16
JSON_NODE_LIMIT = 10000
ENTRY_LIMIT = 128
SAFE_NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]*$")
SAFE_TOKEN = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.+-]*$")
KNOWN_FIELDS = {
    "name",
    "file",
    "sha256",
    "min_daemon_version",
    "requires",
    "hooks",
    "loader",
}
PLACEHOLDERS = {
    "changeme",
    "default",
    "placeholder",
    "replaceme",
    "replacewithrandomsecret",
    "secret",
}
OPEN_DIRECTORY = os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW
OPEN_REGULAR = os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK


class InstallError(Exception):
    """공격자 입력이나 secret을 포함하지 않는 예상 설치 실패다."""


@dataclass(frozen=True)
class Snapshot:
    inode: int
    mtime_ns: int
    size: int
    digest: bytes


def interrupted(_signum, _frame):
    """SIGTERM/SIGINT를 정상 예외 경로로 보내 transaction cleanup을 실행한다."""
    raise InstallError("installation interrupted")


signal.signal(signal.SIGTERM, interrupted)
signal.signal(signal.SIGINT, interrupted)


def fsync_directory(directory_fd):
    os.fsync(directory_fd)


def set_root_owner(file_fd, root_mode):
    """actual root는 root:root, fixture root는 실행 uid:gid를 소유자 계약으로 쓴다."""
    owner_uid = 0 if root_mode else os.geteuid()
    owner_gid = 0 if root_mode else os.getegid()
    os.fchown(file_fd, owner_uid, owner_gid)


@contextmanager
def block_transaction_signals():
    """syscall과 rollback bookkeeping을 signal 관점에서 하나의 원자 구간으로 묶는다."""
    blocked = {signal.SIGINT, signal.SIGTERM}
    previous = signal.pthread_sigmask(signal.SIG_BLOCK, blocked)
    try:
        yield
    finally:
                                                                 
                                                               
        signal.pthread_sigmask(signal.SIG_SETMASK, previous)


def open_directory_path_no_symlinks(path_argument, status):
    """경로의 마지막 component뿐 아니라 모든 ancestor symlink를 거부한다."""
    if not path_argument:
        raise InstallError(status)
    parts = [
        part
        for part in path_argument.split("/")
        if part not in {"", "."}
    ]
    if ".." in parts:
        raise InstallError(status)
    base = "/" if os.path.isabs(path_argument) else "."
    try:
        current_fd = os.open(base, OPEN_DIRECTORY)
        for part in parts:
            next_fd = os.open(part, OPEN_DIRECTORY, dir_fd=current_fd)
            os.close(current_fd)
            current_fd = next_fd
        return current_fd
    except (OSError, ValueError) as exc:
        if "current_fd" in locals():
            os.close(current_fd)
        raise InstallError(status) from exc


def open_root(root_argument):
    root_fd = open_directory_path_no_symlinks(
        root_argument,
        "installation root is not a safe directory",
    )
    slash_fd = os.open("/", OPEN_DIRECTORY)
    try:
        root_stat = os.fstat(root_fd)
        slash_stat = os.fstat(slash_fd)
        root_mode = (
            root_stat.st_dev == slash_stat.st_dev
            and root_stat.st_ino == slash_stat.st_ino
        )
    finally:
        os.close(slash_fd)
    if root_mode and root_argument != "/":
        os.close(root_fd)
        raise InstallError("canonical root requires literal /")
    if root_mode and os.geteuid() != 0:
        os.close(root_fd)
        raise InstallError("actual root installation requires root ownership")

                                                              
                                                    
    fcntl.flock(root_fd, fcntl.LOCK_EX)
    return root_fd, root_mode


def open_stage(stage_argument):
    return open_directory_path_no_symlinks(
        stage_argument,
        "BPF stage is not a safe directory",
    )


def open_child_directory(parent_fd, name, create, mode, root_mode):
    created = False
    try:
        child_fd = os.open(name, OPEN_DIRECTORY, dir_fd=parent_fd)
    except FileNotFoundError:
        if not create:
            raise InstallError("required runtime directory is missing") from None
        try:
            os.mkdir(name, mode=mode, dir_fd=parent_fd)
            created = True
            child_fd = os.open(name, OPEN_DIRECTORY, dir_fd=parent_fd)
        except (OSError, ValueError) as exc:
            raise InstallError("cannot create runtime directory") from exc
    except (OSError, ValueError) as exc:
        raise InstallError("runtime directory is not safe") from exc

    if created:
        os.fchmod(child_fd, mode)
        set_root_owner(child_fd, root_mode)
        fsync_directory(parent_fd)
    return child_fd


def open_directory_chain(root_fd, parts, create, final_mode, root_mode):
    current_fd = os.dup(root_fd)
    try:
        for index, part in enumerate(parts):
            mode = final_mode if index == len(parts) - 1 else 0o755
            next_fd = open_child_directory(
                current_fd,
                part,
                create,
                mode,
                root_mode,
            )
            os.close(current_fd)
            current_fd = next_fd
        return current_fd
    except BaseException:
        os.close(current_fd)
        raise


def read_regular_at(directory_fd, name, size_limit, status):
    try:
        file_fd = os.open(name, OPEN_REGULAR, dir_fd=directory_fd)
    except (OSError, ValueError) as exc:
        raise InstallError(status) from exc

    try:
        file_stat = os.fstat(file_fd)
        if not stat.S_ISREG(file_stat.st_mode):
            raise InstallError(status)
        if file_stat.st_size > size_limit:
            raise InstallError(status)

        chunks = []
        total = 0
        while True:
            chunk = os.read(file_fd, min(1024 * 1024, size_limit + 1 - total))
            if not chunk:
                break
            chunks.append(chunk)
            total += len(chunk)
            if total > size_limit:
                raise InstallError(status)
        content = b"".join(chunks)
        snapshot = Snapshot(
            inode=file_stat.st_ino,
            mtime_ns=file_stat.st_mtime_ns,
            size=file_stat.st_size,
            digest=hashlib.sha256(content).digest(),
        )
        return content, snapshot
    finally:
        os.close(file_fd)


def snapshots_match(left, right):
    return (
        left.inode == right.inode
        and left.mtime_ns == right.mtime_ns
        and left.size == right.size
        and hmac.compare_digest(left.digest, right.digest)
    )


def parse_ini(text):
    """GKeyFile이 소비할 단순 section/key 문법을 보수적으로 검증한다."""
    section_pattern = re.compile(r"^[ \t]*\[([A-Za-z0-9_.-]+)\][ \t]*(?:\r?\n)?$")
    key_pattern = re.compile(
        r"^([ \t]*)([A-Za-z0-9_.-]+)([ \t]*=[ \t]*)(.*?)(\r?\n)?$"
    )
    sections = {}
    current_section = None
    lines = text.splitlines(keepends=True)

    for index, line in enumerate(lines):
        body = line.removesuffix("\n").removesuffix("\r")
        if any(ord(character) < 32 and character != "\t" for character in body):
            raise InstallError("daemon config contains invalid syntax")
        if not body.strip() or body.lstrip().startswith(("#", ";")):
            continue

        section_match = section_pattern.fullmatch(line)
        if section_match:
            current_section = section_match.group(1)
            if current_section in sections:
                raise InstallError("daemon config contains duplicate sections")
            sections[current_section] = {}
            continue

        key_match = key_pattern.fullmatch(line)
        if not key_match or current_section is None:
            raise InstallError("daemon config contains invalid syntax")
        key = key_match.group(2)
        if key in sections[current_section]:
            raise InstallError("daemon config contains duplicate keys")
        sections[current_section][key] = (index, key_match)

    if "daemon" not in sections:
        raise InstallError("daemon config is missing daemon section")
    return lines, sections


def weak_secret(value):
    normalized = re.sub(r"[^a-z0-9]", "", value.lower())
    return len(set(value)) <= 1 or normalized in PLACEHOLDERS


def plan_config_update(config_content):
    try:
        text = config_content.decode("utf-8")
    except UnicodeError as exc:
        raise InstallError("daemon config is not valid UTF-8") from exc
    lines, sections = parse_ini(text)
    jwt_record = sections["daemon"].get("jwt_secret")

    if jwt_record is not None:
        index, match = jwt_record
        existing_value = match.group(4).strip()
        if existing_value:
            if weak_secret(existing_value) or len(existing_value.encode("utf-8")) < 32:
                raise InstallError("daemon JWT is weak; operator action required")
            return None
        generated_secret = secrets.token_hex(32)
        newline = match.group(5) or ""
        lines[index] = (
            f"{match.group(1)}{match.group(2)}{match.group(3)}"
            f"{generated_secret}{newline}"
        )
    else:
        generated_secret = secrets.token_hex(32)
        daemon_keys = sections["daemon"]
        section_header = next(
            index
            for index, line in enumerate(lines)
            if re.fullmatch(
                r"^[ \t]*\[daemon\][ \t]*(?:\r?\n)?$",
                line,
            )
        )
        insertion = len(lines)
        for index in range(section_header + 1, len(lines)):
            if re.fullmatch(
                r"^[ \t]*\[[A-Za-z0-9_.-]+\][ \t]*(?:\r?\n)?$",
                lines[index],
            ):
                insertion = index
                break
        newline = "\r\n" if any(line.endswith("\r\n") for line in lines) else "\n"
        if insertion > 0 and not lines[insertion - 1].endswith(("\n", "\r")):
            lines[insertion - 1] += newline
        lines.insert(insertion, f"jwt_secret={generated_secret}{newline}")
        del daemon_keys

    replacement = "".join(lines).encode("utf-8")
    parse_ini(replacement.decode("utf-8"))
    return replacement


def validate_json_shape(value, depth=0, counter=None):
    if counter is None:
        counter = [0]
    counter[0] += 1
    if counter[0] > JSON_NODE_LIMIT or depth > JSON_DEPTH_LIMIT:
        raise InstallError("BPF manifest exceeds JSON resource limits")
    if isinstance(value, dict):
        for key, child in value.items():
            if not isinstance(key, str):
                raise InstallError("BPF manifest has invalid schema")
            validate_json_shape(child, depth + 1, counter)
    elif isinstance(value, list):
        for child in value:
            validate_json_shape(child, depth + 1, counter)


def validate_string_list(value):
    if not isinstance(value, list):
        raise InstallError("BPF manifest has invalid schema")
    seen = set()
    for item in value:
        if (
            not isinstance(item, str)
            or len(item.encode("utf-8")) >= 64
            or SAFE_TOKEN.fullmatch(item) is None
            or item in seen
        ):
            raise InstallError("BPF manifest has invalid schema")
        seen.add(item)


def validate_bpf_stage(stage_fd):
    manifest_bytes, _snapshot = read_regular_at(
        stage_fd,
        "manifest.json",
        MANIFEST_LIMIT,
        "BPF manifest is not a safe regular file",
    )
    try:
        manifest = json.loads(manifest_bytes.decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError) as exc:
        raise InstallError("BPF manifest is not valid JSON") from exc
    validate_json_shape(manifest)

    if not isinstance(manifest, list) or not manifest or len(manifest) > ENTRY_LIMIT:
        raise InstallError("BPF manifest has invalid schema")

    validated = []
    names = set()
    files = set()
    for entry in manifest:
        if not isinstance(entry, dict) or set(entry) - KNOWN_FIELDS:
            raise InstallError("BPF manifest has invalid schema")
        if not {"name", "file", "sha256"}.issubset(entry):
            raise InstallError("BPF manifest has invalid schema")

        name = entry["name"]
        file_name = entry["file"]
        expected_sha = entry["sha256"]
        if isinstance(file_name, str) and "\x00" in file_name:
            raise InstallError("BPF manifest file value contains NUL")
        if (
            not isinstance(name, str)
            or len(name.encode("utf-8")) >= 64
            or SAFE_NAME.fullmatch(name) is None
            or ".." in name
            or name in names
        ):
            raise InstallError("BPF manifest has invalid schema")
        if (
            not isinstance(file_name, str)
            or len(file_name.encode("utf-8")) >= 128
            or SAFE_NAME.fullmatch(file_name) is None
            or ".." in file_name
            or file_name == "manifest.json"
            or file_name in files
        ):
            raise InstallError("BPF manifest has invalid schema")
        if not isinstance(expected_sha, str) or re.fullmatch(
            r"[0-9a-fA-F]{64}",
            expected_sha,
        ) is None:
            raise InstallError("BPF manifest has invalid schema")
        if "requires" in entry:
            validate_string_list(entry["requires"])
            if not set(entry["requires"]).issubset({"btf", "lsm-bpf"}):
                raise InstallError("BPF manifest has invalid schema")
        if "hooks" in entry:
            validate_string_list(entry["hooks"])
        if "min_daemon_version" in entry:
            version = entry["min_daemon_version"]
            if (
                not isinstance(version, str)
                or len(version.encode("utf-8")) >= 16
                or SAFE_TOKEN.fullmatch(version) is None
            ):
                raise InstallError("BPF manifest has invalid schema")
        if "loader" in entry:
            loader = entry["loader"]
            if loader not in {"auto", "network-tc"}:
                raise InstallError("BPF manifest has invalid schema")

        object_bytes, _object_snapshot = read_regular_at(
            stage_fd,
            file_name,
            OBJECT_LIMIT,
            "BPF stage object is not a safe regular file",
        )
        actual_sha = hashlib.sha256(object_bytes).hexdigest()
        if not hmac.compare_digest(actual_sha, expected_sha.lower()):
            raise InstallError("BPF SHA-256 verification failed")
        names.add(name)
        files.add(file_name)
        validated.append((file_name, object_bytes))
    return manifest_bytes, validated


def unique_transaction_name(prefix):
    return f".pcv-{prefix}-{secrets.token_hex(16)}"


def write_temp_at(directory_fd, target_name, content, mode, root_mode):
    del target_name
    for _attempt in range(16):
        temporary_name = unique_transaction_name("install")
        try:
            temporary_fd = os.open(
                temporary_name,
                os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC | os.O_NOFOLLOW,
                mode,
                dir_fd=directory_fd,
            )
            break
        except FileExistsError:
            continue
        except (OSError, ValueError) as exc:
            raise InstallError("cannot prepare atomic runtime file") from exc
    else:
        raise InstallError("cannot allocate atomic runtime file")

    try:
        os.fchmod(temporary_fd, mode)
        set_root_owner(temporary_fd, root_mode)
        view = memoryview(content)
        while view:
            written = os.write(temporary_fd, view)
            view = view[written:]
        os.fsync(temporary_fd)
    except BaseException:
        os.close(temporary_fd)
        try:
            os.unlink(temporary_name, dir_fd=directory_fd)
        except OSError:
            pass
        raise
    os.close(temporary_fd)
    return temporary_name


def unlink_if_present(directory_fd, name):
    try:
        os.unlink(name, dir_fd=directory_fd)
    except FileNotFoundError:
        pass


def commit_config(config_fd, original_snapshot, replacement, root_mode):
    current_content, current_snapshot = read_regular_at(
        config_fd,
        "daemon.conf",
        MANIFEST_LIMIT,
        "daemon config is not a safe regular file",
    )
    del current_content
    if not snapshots_match(original_snapshot, current_snapshot):
        raise InstallError("daemon config changed during installation")

    if replacement is None:
        file_fd = os.open("daemon.conf", OPEN_REGULAR, dir_fd=config_fd)
        try:
            os.fchmod(file_fd, 0o600)
            set_root_owner(file_fd, root_mode)
            os.fsync(file_fd)
        finally:
            os.close(file_fd)
        fsync_directory(config_fd)
        return

    temporary_name = None
    backup_name = unique_transaction_name("backup")
    backup_created = False
    installed = False
    preserve_backup = False
    try:
        with block_transaction_signals():
            temporary_name = write_temp_at(
                config_fd,
                "daemon.conf",
                replacement,
                0o600,
                root_mode,
            )
        with block_transaction_signals():
            os.link(
                "daemon.conf",
                backup_name,
                src_dir_fd=config_fd,
                dst_dir_fd=config_fd,
                follow_symlinks=False,
            )
            backup_created = True
        _backup_content, backup_snapshot = read_regular_at(
            config_fd,
            backup_name,
            MANIFEST_LIMIT,
            "daemon config changed during installation",
        )
        if not snapshots_match(original_snapshot, backup_snapshot):
            raise InstallError("daemon config changed during installation")

        with block_transaction_signals():
            os.replace(
                temporary_name,
                "daemon.conf",
                src_dir_fd=config_fd,
                dst_dir_fd=config_fd,
            )
            installed = True
            temporary_name = None

                                                          
        _backup_content, final_backup_snapshot = read_regular_at(
            config_fd,
            backup_name,
            MANIFEST_LIMIT,
            "daemon config changed during installation",
        )
        if not snapshots_match(original_snapshot, final_backup_snapshot):
            raise InstallError("daemon config changed during installation")
        fsync_directory(config_fd)
        with block_transaction_signals():
            os.unlink(backup_name, dir_fd=config_fd)
            backup_created = False
        fsync_directory(config_fd)
    except BaseException:
        if installed and backup_created:
            try:
                with block_transaction_signals():
                    os.replace(
                        backup_name,
                        "daemon.conf",
                        src_dir_fd=config_fd,
                        dst_dir_fd=config_fd,
                    )
                    backup_created = False
                    installed = False
                fsync_directory(config_fd)
            except BaseException as rollback_error:
                if backup_created:
                                                                
                                                           
                    preserve_backup = True
                    raise InstallError(
                        "daemon config rollback failed; backup preserved"
                    ) from rollback_error
                raise
        raise
    finally:
        if temporary_name is not None:
            unlink_if_present(config_fd, temporary_name)
        if backup_created and not preserve_backup:
            unlink_if_present(config_fd, backup_name)


def list_existing_regular(destination_fd):
    existing = []
    for name in os.listdir(destination_fd):
        if name.startswith(".pcv-") or SAFE_NAME.fullmatch(name) is None:
            raise InstallError("BPF destination contains an unsafe entry")
        try:
            entry_stat = os.stat(
                name,
                dir_fd=destination_fd,
                follow_symlinks=False,
            )
        except (OSError, ValueError) as exc:
            raise InstallError("BPF destination contains an unsafe entry") from exc
        if not stat.S_ISREG(entry_stat.st_mode):
            raise InstallError("BPF destination contains an unsafe entry")
        existing.append(name)
    return existing


def commit_bpf(destination_fd, manifest_bytes, validated_objects, root_mode):
    existing = list_existing_regular(destination_fd)
    destination_stat = os.fstat(destination_fd)
    os.fchmod(destination_fd, 0o755)
    set_root_owner(destination_fd, root_mode)
    prepared = {}
    backups = {}
    installed = set()
    removed = set()
    committed = False
    target_names = {file_name for file_name, _content in validated_objects}
    target_names.add("manifest.json")

    try:
        for file_name, object_bytes in validated_objects:
            with block_transaction_signals():
                prepared[file_name] = write_temp_at(
                    destination_fd,
                    file_name,
                    object_bytes,
                    0o644,
                    root_mode,
                )
        with block_transaction_signals():
            prepared["manifest.json"] = write_temp_at(
                destination_fd,
                "manifest.json",
                manifest_bytes,
                0o644,
                root_mode,
            )
        fsync_directory(destination_fd)

        for name in existing:
            backup_name = unique_transaction_name("backup")
                                                                      
            backups[name] = backup_name
            with block_transaction_signals():
                os.link(
                    name,
                    backup_name,
                    src_dir_fd=destination_fd,
                    dst_dir_fd=destination_fd,
                    follow_symlinks=False,
                )
        fsync_directory(destination_fd)

        for file_name, _object_bytes in validated_objects:
            with block_transaction_signals():
                os.replace(
                    prepared[file_name],
                    file_name,
                    src_dir_fd=destination_fd,
                    dst_dir_fd=destination_fd,
                )
                prepared.pop(file_name)
                installed.add(file_name)

        for stale_name in set(existing) - target_names:
            with block_transaction_signals():
                os.unlink(stale_name, dir_fd=destination_fd)
                removed.add(stale_name)

                                                                  
                                                   
        fsync_directory(destination_fd)
        with block_transaction_signals():
            os.replace(
                prepared["manifest.json"],
                "manifest.json",
                src_dir_fd=destination_fd,
                dst_dir_fd=destination_fd,
            )
            prepared.pop("manifest.json")
            installed.add("manifest.json")
        fsync_directory(destination_fd)
        committed = True

        for backup_name in backups.values():
            os.unlink(backup_name, dir_fd=destination_fd)
        backups.clear()
        fsync_directory(destination_fd)
    except BaseException:
                                                                  
                                                               
        if not committed:
            for name in installed:
                unlink_if_present(destination_fd, name)
            with block_transaction_signals():
                for name, backup_name in backups.items():
                    try:
                        if name in installed or name in removed:
                            os.replace(
                                backup_name,
                                name,
                                src_dir_fd=destination_fd,
                                dst_dir_fd=destination_fd,
                            )
                        else:
                            os.unlink(backup_name, dir_fd=destination_fd)
                    except OSError:
                        pass
            try:
                os.fchmod(destination_fd, stat.S_IMODE(destination_stat.st_mode))
                if root_mode:
                    os.fchown(
                        destination_fd,
                        destination_stat.st_uid,
                        destination_stat.st_gid,
                    )
            except OSError:
                pass
        else:
            for backup_name in backups.values():
                unlink_if_present(destination_fd, backup_name)
        backups.clear()
        try:
            fsync_directory(destination_fd)
        except OSError:
            pass
        raise
    finally:
        for temporary_name in prepared.values():
            unlink_if_present(destination_fd, temporary_name)
        for backup_name in backups.values():
            unlink_if_present(destination_fd, backup_name)


def main():
    root_fd = None
    stage_fd = None
    config_fd = None
    pki_fd = None
    library_fd = None
    destination_fd = None
    try:
        verify_only = sys.argv[3] == "1"
        if verify_only:
                                                         
                                                                 
            stage_fd = open_stage(sys.argv[2])
            validate_bpf_stage(stage_fd)
            return True

        root_fd, root_mode = open_root(sys.argv[1])
        stage_fd = open_stage(sys.argv[2])

        config_fd = open_directory_chain(
            root_fd,
            ("etc", "purecvisor"),
            False,
            0o755,
            root_mode,
        )
        config_content, config_snapshot = read_regular_at(
            config_fd,
            "daemon.conf",
            MANIFEST_LIMIT,
            "required daemon config is missing or unsafe",
        )
        config_replacement = plan_config_update(config_content)
        manifest_bytes, validated_objects = validate_bpf_stage(stage_fd)

                                                       
        os.fchmod(config_fd, 0o755)
        set_root_owner(config_fd, root_mode)
        fsync_directory(config_fd)
        pki_fd = open_child_directory(
            config_fd,
            "pki",
            True,
            0o700,
            root_mode,
        )
        os.fchmod(pki_fd, 0o700)
        set_root_owner(pki_fd, root_mode)
        fsync_directory(config_fd)

        library_fd = open_directory_chain(
            root_fd,
            ("usr", "lib", "purecvisor"),
            True,
            0o755,
            root_mode,
        )
        os.fchmod(library_fd, 0o755)
        set_root_owner(library_fd, root_mode)
        fsync_directory(library_fd)
        destination_fd = open_child_directory(
            library_fd,
            "bpf",
            True,
            0o755,
            root_mode,
        )
        commit_config(
            config_fd,
            config_snapshot,
            config_replacement,
            root_mode,
        )
        commit_bpf(
            destination_fd,
            manifest_bytes,
            validated_objects,
            root_mode,
        )
        fsync_directory(root_fd)
        return False
    finally:
        for file_fd in (
            destination_fd,
            library_fd,
            pki_fd,
            config_fd,
            stage_fd,
            root_fd,
        ):
            if file_fd is not None:
                os.close(file_fd)


try:
    verified_only = main()
except InstallError as exc:
    print(f"error: {exc}", file=sys.stderr)
    raise SystemExit(1) from None
except Exception:
                                                           
                                                               
    print("error: runtime prerequisite installation failed", file=sys.stderr)
    raise SystemExit(1) from None

if verified_only:
    print("runtime prerequisites: verified")
else:
    print("runtime prerequisites: installed")
    print("config: /etc/purecvisor/daemon.conf")
    print("bpf: /usr/lib/purecvisor/bpf")
PY
