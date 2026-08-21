#!/usr/bin/env python3
                                                                                         
                                                                                 
                                                               
                                                 

from __future__ import annotations

import fcntl
import hashlib
import json
import os
import pathlib
import re
import shutil
import signal
import stat
import subprocess
import sys
import time
import types
from typing import Any


HELPER = pathlib.Path(sys.argv[1])
STATE = pathlib.Path(sys.argv[2])
VALID_SECRET = (
    "0123456789abcdef0123456789abcdef"
    "fedcba9876543210fedcba9876543210"
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def config_bytes(secret: str = "", newline: bytes = b"\n") -> bytes:
    return newline.join(
        [
            b"# security fixture comment",
            b"[daemon]",
            b"rest_port=8080",
            f"jwt_secret={secret}".encode(),
            b"",
            b"[security_group]",
            b"resync_interval_sec=300",
            b"",
        ]
    )


def make_root(name: str, config: bytes | None = None) -> pathlib.Path:
    root = STATE / name
    config_dir = root / "etc/purecvisor"
    config_dir.mkdir(parents=True)
    (config_dir / "daemon.conf").write_bytes(
        config if config is not None else config_bytes()
    )
    return root


def manifest_entry(
    name: str,
    file_name: str,
    content: bytes,
    **updates: Any,
) -> dict[str, Any]:
    entry: dict[str, Any] = {
        "name": name,
        "file": file_name,
        "sha256": hashlib.sha256(content).hexdigest(),
        "min_daemon_version": "2.0",
        "requires": ["btf", "lsm-bpf"],
        "hooks": ["file_open"],
    }
    entry.update(updates)
    return entry


def make_stage(
    name: str,
    objects: dict[str, bytes] | None = None,
    manifest: Any | None = None,
) -> pathlib.Path:
    stage = STATE / name
    stage.mkdir(parents=True)
    if objects is None:
        objects = {"pcv_lsm.bpf.o": b"security-fixture-object\n"}
    for file_name, content in objects.items():
        (stage / file_name).write_bytes(content)
    if manifest is None:
        file_name, content = next(iter(objects.items()))
        manifest = [manifest_entry("pcv_lsm", file_name, content)]
    (stage / "manifest.json").write_text(
        json.dumps(manifest, separators=(",", ":")),
        encoding="utf-8",
    )
    return stage


def invoke(
    root: pathlib.Path | str,
    stage: pathlib.Path | str,
    *,
    timeout: float = 8.0,
    umask: int | None = None,
) -> subprocess.CompletedProcess[str]:
    preexec_fn = None
    if umask is not None:
        preexec_fn = lambda: os.umask(umask)
    try:
        return subprocess.run(
            [str(HELPER), "--root", str(root), "--bpf-stage", str(stage)],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
            check=False,
            preexec_fn=preexec_fn,
        )
    except subprocess.TimeoutExpired as exc:
        raise SystemExit("FAIL: runtime prerequisite helper exceeded bounded time") from exc


def safe_failure(result: subprocess.CompletedProcess[str], label: str) -> None:
    output = result.stdout + result.stderr
    require(result.returncode != 0, f"{label} must fail closed")
    require(len(output.splitlines()) == 1, f"{label} must emit one safe status line")
    require(
        not re.search(r"Traceback|ValueError|Exception", output),
        f"{label} must not expose a Python exception",
    )


def fingerprint(path: pathlib.Path) -> list[tuple[str, str, int]]:
    if not path.exists():
        return []
    result = []
    for candidate in sorted(path.rglob("*")):
        relative = str(candidate.relative_to(path))
        mode = stat.S_IMODE(candidate.lstat().st_mode)
        if candidate.is_symlink():
            digest = f"symlink:{os.readlink(candidate)}"
        elif candidate.is_file():
            digest = hashlib.sha256(candidate.read_bytes()).hexdigest()
        elif candidate.is_dir():
            digest = "directory"
        else:
            digest = "special"
        result.append((relative, digest, mode))
    return result


def read_jwt(root: pathlib.Path) -> str:
    text = (root / "etc/purecvisor/daemon.conf").read_text(encoding="utf-8")
    found = re.findall(r"(?m)^[ \t]*jwt_secret[ \t]*=[ \t]*(.*)$", text)
    require(len(found) == 1, "config must contain exactly one JWT")
    return found[0].strip()


def process_tree_pids(root_pid: int) -> list[int]:
                                                          
    pending = [root_pid]
    observed = []
    while pending:
        process_id = pending.pop()
        if process_id in observed:
            continue
        observed.append(process_id)
        children_path = pathlib.Path(
            f"/proc/{process_id}/task/{process_id}/children"
        )
        try:
            pending.extend(
                int(value) for value in children_path.read_text().split()
            )
        except (FileNotFoundError, PermissionError, OSError):
            pass
    return observed


def load_installer_module() -> types.ModuleType:
                                                                      
    source = HELPER.read_text(encoding="utf-8")
    marker = re.search(r"^exec python3 - .* <<'PY'\n", source, re.MULTILINE)
    require(marker is not None, "fixture must locate embedded installer Python")
    embedded = source[marker.end() :].rsplit("\nPY\n", 1)[0]
    definitions = embedded.split("\ntry:\n", 1)[0]
    module_name = "_pcv_runtime_prereq_embedded"
    module = types.ModuleType(module_name)
    sys.modules[module_name] = module
    exec(compile(definitions, str(HELPER), "exec"), module.__dict__)
    return module


STATE.mkdir(parents=True)
valid_stage = make_stage("valid-stage")

                                                
symlink_target = make_root("symlink-target")
symlink_root = STATE / "symlink-root"
symlink_root.symlink_to(symlink_target, target_is_directory=True)
safe_failure(invoke(symlink_root, valid_stage), "symlink root")
alias_result = invoke("//", STATE / "missing-stage")
safe_failure(alias_result, "non-literal canonical root")
require(
    alias_result.stderr.strip() == "error: canonical root requires literal /",
    "canonical root alias must be rejected before stage access",
)

                                                         
                      
swap_root = make_root("swap-root")
outside = make_root("swap-outside", config_bytes(VALID_SECRET))
outside_before = fingerprint(outside)
lock_fd = os.open(swap_root, os.O_RDONLY | os.O_DIRECTORY)
fcntl.flock(lock_fd, fcntl.LOCK_EX)
swap_process = subprocess.Popen(
    [str(HELPER), "--root", str(swap_root), "--bpf-stage", str(valid_stage)],
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    text=True,
)
time.sleep(0.15)
require(swap_process.poll() is None, "helper must wait on the root-scoped flock")
held_root = STATE / "swap-root-held"
swap_root.rename(held_root)
swap_root.symlink_to(outside, target_is_directory=True)
fcntl.flock(lock_fd, fcntl.LOCK_UN)
os.close(lock_fd)
swap_stdout, swap_stderr = swap_process.communicate(timeout=8)
require(swap_process.returncode == 0, "root swap-safe install must succeed")
require(not swap_stderr, "root swap-safe install must not emit stderr")
require(len(swap_stdout.splitlines()) == 3, "success output must contain status/paths")
require(fingerprint(outside) == outside_before, "root swap must not write outside root FD")
require(len(read_jwt(held_root)) == 64, "root FD target must receive generated JWT")

                                                  
concurrent_root = make_root("concurrent-root")
processes = [
    subprocess.Popen(
        [str(HELPER), "--root", str(concurrent_root), "--bpf-stage", str(valid_stage)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    for _ in range(2)
]
for process in processes:
    process.communicate(timeout=8)
    require(process.returncode == 0, "concurrent installs must both succeed")
concurrent_jwt = read_jwt(concurrent_root)
require(
    re.fullmatch(r"[0-9a-f]{64}", concurrent_jwt) is not None,
    "concurrent installs must leave one generated JWT",
)

                                                        
edit_root = make_root("operator-edit-root")
large_content = b"x" * (64 * 1024 * 1024)
edit_stage = make_stage(
    "operator-edit-stage",
    {"pcv_lsm.bpf.o": large_content},
)
edit_process = subprocess.Popen(
    [str(HELPER), "--root", str(edit_root), "--bpf-stage", str(edit_stage)],
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    text=True,
)
object_path = str(edit_stage / "pcv_lsm.bpf.o")
deadline = time.monotonic() + 4
observed_read = False
while time.monotonic() < deadline and edit_process.poll() is None:
    observed_read = False
    for process_id in process_tree_pids(edit_process.pid):
        fd_dir = pathlib.Path(f"/proc/{process_id}/fd")
        try:
            observed_read = any(
                os.readlink(fd) == object_path for fd in fd_dir.iterdir()
            )
        except (FileNotFoundError, PermissionError, OSError):
            observed_read = False
        if observed_read:
            break
    if observed_read:
        break
    time.sleep(0.001)
require(observed_read, "fixture must observe BPF validation after config snapshot")
operator_edit = config_bytes(VALID_SECRET)
(edit_root / "etc/purecvisor/daemon.conf").write_bytes(operator_edit)
edit_stdout, edit_stderr = edit_process.communicate(timeout=8)
safe_failure(
    subprocess.CompletedProcess([], edit_process.returncode, edit_stdout, edit_stderr),
    "concurrent operator edit",
)
require(
    (edit_root / "etc/purecvisor/daemon.conf").read_bytes() == operator_edit,
    "installer must not overwrite a concurrent operator edit",
)
del large_content

                                            
for index, weak_secret in enumerate(
    [
        "0" * 64,
        "a" * 64,
        "changeme",
        "replace-with-random-secret",
    ]
):
    weak_root = make_root(f"weak-root-{index}", config_bytes(weak_secret))
    before = fingerprint(weak_root)
    safe_failure(invoke(weak_root, valid_stage), f"weak JWT {index}")
    require(fingerprint(weak_root) == before, "weak JWT failure must change nothing")

                                                          
invalid_configs = [
    b"[daemon]\njwt_secret=\nmalformed line\n",
    b"[daemon]\njwt_secret=\n[daemon]\nrest_port=8080\n",
    b"[daemon]\njwt_secret=\njwt_secret=another\n",
]
for index, invalid_config in enumerate(invalid_configs):
    invalid_root = make_root(f"invalid-ini-{index}", invalid_config)
    before = fingerprint(invalid_root)
    safe_failure(invoke(invalid_root, valid_stage), f"invalid INI {index}")
    require(fingerprint(invalid_root) == before, "invalid INI must change nothing")

crlf_root = make_root("crlf-root", config_bytes("", b"\r\n"))
crlf_result = invoke(crlf_root, valid_stage)
require(crlf_result.returncode == 0, "valid CRLF config must install")
crlf_after = (crlf_root / "etc/purecvisor/daemon.conf").read_bytes()
require(b"# security fixture comment\r\n" in crlf_after, "CRLF comment must survive")
require(re.search(rb"(?<!\r)\n", crlf_after) is None, "CRLF style must be preserved")

                                                                       
schema_cases: list[tuple[str, Any]] = [
    ("empty", []),
    (
        "name-traversal",
        [manifest_entry("../pcv", "pcv_lsm.bpf.o", b"security-fixture-object\n")],
    ),
    (
        "duplicate-name",
        [
            manifest_entry("same", "one.bpf.o", b"one"),
            manifest_entry("same", "two.bpf.o", b"two"),
        ],
    ),
    (
        "duplicate-file",
        [
            manifest_entry("one", "same.bpf.o", b"same"),
            manifest_entry("two", "same.bpf.o", b"same"),
        ],
    ),
    (
        "control-name",
        [manifest_entry("bad\nname", "pcv_lsm.bpf.o", b"security-fixture-object\n")],
    ),
    (
        "surrogate-name",
        [manifest_entry("\ud800", "pcv_lsm.bpf.o", b"security-fixture-object\n")],
    ),
    (
        "missing-sha",
        [
            {
                "name": "pcv",
                "file": "pcv_lsm.bpf.o",
                "requires": ["btf"],
            }
        ],
    ),
    (
        "bad-requires",
        [
            manifest_entry(
                "pcv",
                "pcv_lsm.bpf.o",
                b"security-fixture-object\n",
                requires="btf",
            )
        ],
    ),
]
for label, manifest in schema_cases:
    objects = {
        "pcv_lsm.bpf.o": b"security-fixture-object\n",
        "one.bpf.o": b"one",
        "two.bpf.o": b"two",
        "same.bpf.o": b"same",
    }
    schema_stage = make_stage(f"schema-{label}", objects, manifest)
    schema_root = make_root(f"schema-root-{label}", config_bytes(VALID_SECRET))
    before = fingerprint(schema_root)
    result = invoke(schema_root, schema_stage)
    safe_failure(result, f"manifest schema {label}")
    require(fingerprint(schema_root) == before, "invalid manifest must change nothing")
    if label == "control-name":
        require("bad" not in result.stderr, "control filename must not enter logs")

oversize_stage = make_stage("oversize-stage")
(oversize_stage / "manifest.json").write_bytes(b" " * (1024 * 1024 + 1) + b"[]")
safe_failure(
    invoke(make_root("oversize-root", config_bytes(VALID_SECRET)), oversize_stage),
    "oversize manifest",
)
deep_stage = make_stage("deep-stage")
deep_value: Any = "leaf"
for _ in range(24):
    deep_value = [deep_value]
deep_manifest = manifest_entry(
    "pcv_lsm",
    "pcv_lsm.bpf.o",
    b"security-fixture-object\n",
    hooks=deep_value,
)
(deep_stage / "manifest.json").write_text(json.dumps([deep_manifest]), encoding="utf-8")
safe_failure(
    invoke(make_root("deep-root", config_bytes(VALID_SECRET)), deep_stage),
    "deep manifest",
)

                                                             
fifo_stage = STATE / "fifo-stage"
fifo_stage.mkdir()
os.mkfifo(fifo_stage / "pcv_fifo.bpf.o")
(fifo_stage / "manifest.json").write_text(
    json.dumps(
        [
            {
                "name": "pcv_fifo",
                "file": "pcv_fifo.bpf.o",
                "sha256": "1" * 64,
                "requires": ["btf"],
            }
        ]
    ),
    encoding="utf-8",
)
fifo_started = time.monotonic()
safe_failure(
    invoke(
        make_root("fifo-root", config_bytes(VALID_SECRET)),
        fifo_stage,
        timeout=2,
    ),
    "FIFO stage object",
)
require(time.monotonic() - fifo_started < 2, "FIFO rejection must be bounded")

                                                                    
failure_root = make_root("failure-root", config_bytes(VALID_SECRET))
initial_result = invoke(failure_root, valid_stage)
require(initial_result.returncode == 0, "failure fixture baseline install must succeed")
destination = failure_root / "usr/lib/purecvisor/bpf"

                                                                 
signal_content = b"s" * (64 * 1024 * 1024)
signal_stage = make_stage(
    "signal-stage",
    {"pcv_lsm.bpf.o": signal_content},
)
signal_before = fingerprint(destination)
signal_process = subprocess.Popen(
    [str(HELPER), "--root", str(failure_root), "--bpf-stage", str(signal_stage)],
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    text=True,
)
deadline = time.monotonic() + 8
temporary_seen = False
while time.monotonic() < deadline and signal_process.poll() is None:
    temporary_seen = any(item.name.startswith(".pcv-") for item in destination.iterdir())
    if temporary_seen:
        break
    time.sleep(0.001)
require(temporary_seen, "signal fixture must observe a transaction temp file")
signal_process.send_signal(signal.SIGTERM)
signal_stdout, signal_stderr = signal_process.communicate(timeout=8)
require(signal_process.returncode != 0, "SIGTERM must stop the install")
require(
    "Traceback" not in signal_stdout + signal_stderr,
    "SIGTERM must not emit a traceback",
)
require(
    fingerprint(destination) == signal_before,
    "SIGTERM must preserve the prior BPF installation",
)
require(
    not any(item.name.startswith(".pcv-") for item in destination.iterdir()),
    "SIGTERM must clean transaction files",
)

                                              
umask_root = make_root("umask-root")
umask_result = invoke(umask_root, valid_stage, umask=0)
require(umask_result.returncode == 0, "umask 000 install must succeed")
expected_modes = {
    umask_root / "etc/purecvisor/pki": 0o700,
    umask_root / "etc/purecvisor/daemon.conf": 0o600,
    umask_root / "usr": 0o755,
    umask_root / "usr/lib": 0o755,
    umask_root / "usr/lib/purecvisor": 0o755,
    umask_root / "usr/lib/purecvisor/bpf": 0o755,
    umask_root / "usr/lib/purecvisor/bpf/pcv_lsm.bpf.o": 0o644,
    umask_root / "usr/lib/purecvisor/bpf/manifest.json": 0o644,
}
for path, expected_mode in expected_modes.items():
    require(
        stat.S_IMODE(path.stat().st_mode) == expected_mode,
        f"explicit mode must survive umask 000: {path.name}",
    )

                                                             
ancestor_root = make_root("ancestor-root")
(ancestor_root / "etc/purecvisor").chmod(0o777)
(ancestor_root / "usr/lib/purecvisor/bpf").mkdir(parents=True)
(ancestor_root / "usr/lib/purecvisor").chmod(0o777)
(ancestor_root / "usr/lib/purecvisor/bpf").chmod(0o777)
ancestor_result = invoke(ancestor_root, valid_stage)
require(ancestor_result.returncode == 0, "insecure managed ancestors must be corrected")
for managed_path in (
    ancestor_root / "etc/purecvisor",
    ancestor_root / "usr/lib/purecvisor",
    ancestor_root / "usr/lib/purecvisor/bpf",
):
    managed_stat = managed_path.stat()
    require(
        stat.S_IMODE(managed_stat.st_mode) == 0o755,
        "managed ancestor mode must be corrected to 0755",
    )
    require(
        (managed_stat.st_uid, managed_stat.st_gid) == (os.geteuid(), os.getegid()),
        "fixture managed ancestor owner must use the current uid/gid root surrogate",
    )

                                                                
old_sigterm = signal.getsignal(signal.SIGTERM)
old_sigint = signal.getsignal(signal.SIGINT)
installer = load_installer_module()
try:
                                                                
    failure_before = fingerprint(destination)
    failure_manifest_before = (destination / "manifest.json").read_bytes()
    failure_fd = os.open(destination, os.O_RDONLY | os.O_DIRECTORY)
    original_replace = installer.os.replace
    mutation_events: list[str] = []
    mutation_failed = False

    def fail_second_object_replace(
        source: str,
        target: str,
        *args: Any,
        **kwargs: Any,
    ) -> Any:
        mutation_events.append(f"replace:{target}")
        if target == "second.bpf.o":
            raise OSError("injected object replace failure")
        return original_replace(source, target, *args, **kwargs)

    installer.os.replace = fail_second_object_replace
    try:
        try:
            installer.commit_bpf(
                failure_fd,
                b"replacement-manifest",
                [
                    ("pcv_lsm.bpf.o", b"replacement-object"),
                    ("second.bpf.o", b"second-object"),
                ],
                False,
            )
        except (OSError, installer.InstallError):
            mutation_failed = True
    finally:
        installer.os.replace = original_replace
        os.close(failure_fd)
    require(mutation_failed, "BPF mutation fault must fail the transaction")
    first_mutation = mutation_events.index("replace:pcv_lsm.bpf.o")
    injected_failure = mutation_events.index("replace:second.bpf.o")
    require(
        first_mutation < injected_failure,
        "BPF fault must occur after at least one object mutation",
    )
    require(
        "replace:manifest.json" not in mutation_events[:injected_failure + 1],
        "manifest must not commit before every object mutation succeeds",
    )
    require(
        fingerprint(destination) == failure_before,
        "BPF mutation failure must restore the complete prior generation",
    )
    require(
        (destination / "manifest.json").read_bytes() == failure_manifest_before,
        "BPF mutation failure must restore the prior manifest commit marker",
    )
    require(
        not any(item.name.startswith(".pcv-") for item in destination.iterdir()),
        "BPF mutation failure must remove temp and backup artifacts",
    )

                                                                       
    link_dir = STATE / "link-signal"
    link_dir.mkdir()
    link_config = config_bytes("")
    (link_dir / "daemon.conf").write_bytes(link_config)
    link_fd = os.open(link_dir, os.O_RDONLY | os.O_DIRECTORY)
    _content, link_snapshot = installer.read_regular_at(
        link_fd,
        "daemon.conf",
        installer.MANIFEST_LIMIT,
        "fixture",
    )
    original_link = installer.os.link
    link_signalled = False

    def link_then_signal(*args: Any, **kwargs: Any) -> Any:
        global link_signalled
        result = original_link(*args, **kwargs)
        if not link_signalled:
            link_signalled = True
            os.kill(os.getpid(), signal.SIGTERM)
        return result

    installer.os.link = link_then_signal
    try:
        try:
            installer.commit_config(
                link_fd,
                link_snapshot,
                config_bytes(VALID_SECRET),
                False,
            )
        except installer.InstallError:
            pass
    finally:
        installer.os.link = original_link
        os.close(link_fd)
    require(
        (link_dir / "daemon.conf").read_bytes() == link_config,
        "link-post-signal must preserve the original config",
    )
    require(
        not any(item.name.startswith(".pcv-") for item in link_dir.iterdir()),
        "link-post-signal must leave no transaction artifact",
    )

                                                                    
    replace_dir = STATE / "replace-signal"
    replace_dir.mkdir()
    (replace_dir / "manifest.json").write_bytes(b"old-manifest")
    replace_before = fingerprint(replace_dir)
    replace_fd = os.open(replace_dir, os.O_RDONLY | os.O_DIRECTORY)
    original_replace = installer.os.replace
    replace_signalled = False

    def replace_then_signal(
        source: str,
        target: str,
        *args: Any,
        **kwargs: Any,
    ) -> Any:
        global replace_signalled
        result = original_replace(source, target, *args, **kwargs)
        if target == "new.bpf.o" and not replace_signalled:
            replace_signalled = True
            os.kill(os.getpid(), signal.SIGTERM)
        return result

    installer.os.replace = replace_then_signal
    try:
        try:
            installer.commit_bpf(
                replace_fd,
                b"new-manifest",
                [("new.bpf.o", b"new-object")],
                False,
            )
        except installer.InstallError:
            pass
    finally:
        installer.os.replace = original_replace
        os.close(replace_fd)
    require(
        fingerprint(replace_dir) == replace_before,
        "replace-post-signal must rollback a newly installed object",
    )
    require(
        not any(item.name.startswith(".pcv-") for item in replace_dir.iterdir()),
        "replace-post-signal must leave no transaction artifact",
    )

                                                                           
    close_config_dir = STATE / "close-signal-config"
    close_config_dir.mkdir()
    close_config_original = config_bytes("")
    (close_config_dir / "daemon.conf").write_bytes(close_config_original)
    close_config_fd = os.open(close_config_dir, os.O_RDONLY | os.O_DIRECTORY)
    _content, close_config_snapshot = installer.read_regular_at(
        close_config_fd,
        "daemon.conf",
        installer.MANIFEST_LIMIT,
        "fixture",
    )
    original_close = installer.os.close
    close_signalled = False

    def close_then_signal(file_fd: int) -> None:
        global close_signalled
        try:
            file_name = pathlib.Path(os.readlink(f"/proc/self/fd/{file_fd}")).name
        except OSError:
            file_name = ""
        original_close(file_fd)
        if file_name.startswith(".pcv-install-") and not close_signalled:
            close_signalled = True
            os.kill(os.getpid(), signal.SIGTERM)

    installer.os.close = close_then_signal
    try:
        try:
            installer.commit_config(
                close_config_fd,
                close_config_snapshot,
                config_bytes(VALID_SECRET),
                False,
            )
        except installer.InstallError:
            pass
    finally:
        installer.os.close = original_close
        os.close(close_config_fd)
    require(
        (close_config_dir / "daemon.conf").read_bytes() == close_config_original,
        "temp-close signal must preserve the existing config",
    )
    require(
        not any(item.name.startswith(".pcv-") for item in close_config_dir.iterdir()),
        "temp-close signal must remove config transaction files",
    )

    close_bpf_dir = STATE / "close-signal-bpf"
    close_bpf_dir.mkdir()
    (close_bpf_dir / "manifest.json").write_bytes(b"old-manifest")
    close_bpf_before = fingerprint(close_bpf_dir)
    close_bpf_fd = os.open(close_bpf_dir, os.O_RDONLY | os.O_DIRECTORY)
    close_signalled = False
    installer.os.close = close_then_signal
    try:
        try:
            installer.commit_bpf(
                close_bpf_fd,
                b"new-manifest",
                [("new.bpf.o", b"new-object")],
                False,
            )
        except installer.InstallError:
            pass
    finally:
        installer.os.close = original_close
        os.close(close_bpf_fd)
    require(
        fingerprint(close_bpf_dir) == close_bpf_before,
        "temp-close signal must preserve the existing BPF tree",
    )
    require(
        not any(item.name.startswith(".pcv-") for item in close_bpf_dir.iterdir()),
        "temp-close signal must remove BPF transaction files",
    )

                                                                
    rollback_dir = STATE / "rollback-failure"
    rollback_dir.mkdir()
    rollback_original = config_bytes("")
    (rollback_dir / "daemon.conf").write_bytes(rollback_original)
    rollback_fd = os.open(rollback_dir, os.O_RDONLY | os.O_DIRECTORY)
    _content, rollback_snapshot = installer.read_regular_at(
        rollback_fd,
        "daemon.conf",
        installer.MANIFEST_LIMIT,
        "fixture",
    )
    original_fsync_directory = installer.fsync_directory
    original_replace = installer.os.replace
    fsync_injected = False

    def fail_after_install(_directory_fd: int) -> None:
        global fsync_injected
        if not fsync_injected:
            fsync_injected = True
            raise installer.InstallError("injected commit failure")
        original_fsync_directory(_directory_fd)

    def fail_rollback_replace(
        source: str,
        target: str,
        *args: Any,
        **kwargs: Any,
    ) -> Any:
        if source.startswith(".pcv-backup-") and target == "daemon.conf":
            raise OSError("injected rollback failure")
        return original_replace(source, target, *args, **kwargs)

    installer.fsync_directory = fail_after_install
    installer.os.replace = fail_rollback_replace
    rollback_error = ""
    try:
        try:
            installer.commit_config(
                rollback_fd,
                rollback_snapshot,
                config_bytes(VALID_SECRET),
                False,
            )
        except installer.InstallError as exc:
            rollback_error = str(exc)
    finally:
        installer.fsync_directory = original_fsync_directory
        installer.os.replace = original_replace
    backups = [
        item for item in rollback_dir.iterdir()
        if item.name.startswith(".pcv-backup-")
    ]
    require(
        rollback_error == "daemon config rollback failed; backup preserved",
        "rollback failure must return one fixed safe recovery status",
    )
    require(len(backups) == 1, "rollback failure must preserve exactly one backup")
    require(
        backups[0].read_bytes() == rollback_original,
        "preserved rollback backup must contain the recoverable original config",
    )
    original_replace(
        backups[0].name,
        "daemon.conf",
        src_dir_fd=rollback_fd,
        dst_dir_fd=rollback_fd,
    )
    require(
        (rollback_dir / "daemon.conf").read_bytes() == rollback_original,
        "preserved backup must restore the original config",
    )
    os.close(rollback_fd)

                                                                        
    order_dir = STATE / "fsync-order"
    order_dir.mkdir()
    (order_dir / "stale.bpf.o").write_bytes(b"stale")
    (order_dir / "manifest.json").write_bytes(b"old-manifest")
    order_fd = os.open(order_dir, os.O_RDONLY | os.O_DIRECTORY)
    original_replace = installer.os.replace
    original_fsync_directory = installer.fsync_directory
    events: list[str] = []

    def record_replace(
        source: str,
        target: str,
        *args: Any,
        **kwargs: Any,
    ) -> Any:
        events.append(f"replace:{target}")
        return original_replace(source, target, *args, **kwargs)

    def record_fsync(directory_fd: int) -> None:
        events.append("fsync")
        original_fsync_directory(directory_fd)

    installer.os.replace = record_replace
    installer.fsync_directory = record_fsync
    try:
        installer.commit_bpf(
            order_fd,
            b"new-manifest",
            [("new.bpf.o", b"new-object")],
            False,
        )
    finally:
        installer.os.replace = original_replace
        installer.fsync_directory = original_fsync_directory
        os.close(order_fd)
    object_index = events.index("replace:new.bpf.o")
    manifest_index = events.index("replace:manifest.json")
    require(
        object_index < manifest_index,
        "successful BPF transaction must replace manifest last",
    )
    require(
        "fsync" in events[object_index + 1:manifest_index],
        "object rename and stale cleanup must be fsynced before manifest rename",
    )
    require(
        "fsync" in events[manifest_index + 1:],
        "manifest rename must be followed by a directory fsync",
    )
finally:
    signal.signal(signal.SIGTERM, old_sigterm)
    signal.signal(signal.SIGINT, old_sigint)

print("PASS: runtime prerequisite security cases")
