#!/usr/bin/env python3
                                                                              
                                                                         
                                                             
                                                          

from __future__ import annotations

import configparser
import json
import os
import pathlib
import re
import secrets
import shutil
import stat
import subprocess
import sys
import tempfile
import time


for _sudo_key in ("SUDO_UID", "SUDO_GID", "SUDO_USER", "SUDO_COMMAND"):
    os.environ.pop(_sudo_key, None)

INSTALLER = pathlib.Path(sys.argv[1]).resolve()
WAIT_HELPER = pathlib.Path(sys.argv[2]).resolve()


def run(
    root: pathlib.Path, *args: str, check: bool = True, installer: pathlib.Path | None = None
) -> subprocess.CompletedProcess[str]:
    safe_helper = root / ".fixture-wait-for-local-ip"
    if not safe_helper.exists():
        safe_helper.write_bytes(WAIT_HELPER.read_bytes())
        safe_helper.chmod(0o700)
    return subprocess.run(
        [str(installer or INSTALLER), "--root", str(root), *args],
        text=True,
        capture_output=True,
        check=check,
        env={**os.environ, "PCV_WAIT_FOR_LOCAL_IP_SOURCE": str(safe_helper)},
    )


def seed(root: pathlib.Path) -> None:
    (root / "etc/purecvisor").mkdir(parents=True)
    (root / "usr/lib/systemd/system").mkdir(parents=True)
    (root / "etc/purecvisor/daemon.conf").write_text(
        "[unknown]\nkeep=yes\n\n[tls]\nhttps_enabled=true\n\n"
        "[server]\nbind_plaintext=all\nextra=preserved\n",
        encoding="utf-8",
    )
    (root / "usr/lib/systemd/system/nginx.service").write_text(
        "[Unit]\nDescription=fixture nginx\n"
        "[Service]\nType=forking\n"
        "ExecStartPre=/usr/sbin/nginx -t -q -g 'daemon on; master_process on;'\n"
        "ExecStart=/usr/sbin/nginx -g 'daemon on; master_process on;'\n",
        encoding="utf-8",
    )


def assert_regular(path: pathlib.Path, mode: int) -> None:
    info = path.lstat()
    assert stat.S_ISREG(info.st_mode), path
    assert stat.S_IMODE(info.st_mode) == mode, (path, oct(stat.S_IMODE(info.st_mode)))


def install_and_id(root: pathlib.Path) -> str:
    result = run(root, "--nginx-bind-ip", "192.0.2.73")
    prefix = "PCV_NGINX_DEPLOYMENT_ID="
    assert result.stdout.startswith(prefix), result
    deployment_id = result.stdout.strip().removeprefix(prefix)
    assert len(deployment_id) == 64 and all(c in "0123456789abcdef" for c in deployment_id)
    return deployment_id


def transaction_state(root: pathlib.Path, intent: str, deployment_id: str) -> pathlib.Path:
    return root / (
        "var/lib/purecvisor/nginx-termination-transactions/"
        f"{intent}.{deployment_id}"
    )


def rollback_and_finalize(root: pathlib.Path, deployment_id: str) -> None:
    run(root, "--rollback", deployment_id)
    assert transaction_state(root, "rolledback", deployment_id).is_dir()
    run(root, "--finalize-rollback", deployment_id)
    assert not transaction_state(root, "rolledback", deployment_id).exists()


def basic_install_and_rollback() -> None:
    with tempfile.TemporaryDirectory(prefix="pcv-nginx-install.") as directory:
        root = pathlib.Path(directory)
        seed(root)
        before = (root / "etc/purecvisor/daemon.conf").read_bytes()
        before_mode = stat.S_IMODE((root / "etc/purecvisor/daemon.conf").stat().st_mode)
        deployment_id = install_and_id(root)
        helper = root / "usr/local/libexec/purecvisor/wait-for-local-ip"
        dropin = root / "etc/systemd/system/nginx.service.d/50-purecvisor-bind-ready.conf"
        assert_regular(helper, 0o755)
        assert_regular(dropin, 0o644)
        expected = (
            "[Unit]\nStartLimitIntervalSec=0\n\n[Service]\nExecStartPre=\n"
            "ExecStartPre=/usr/local/libexec/purecvisor/wait-for-local-ip 192.0.2.73 60\n"
            "ExecStartPre=/usr/sbin/nginx -t -q -g 'daemon on; master_process on;'\n"
            "Restart=on-failure\nRestartPreventExitStatus=1\nRestartSec=5s\n"
        )
        assert dropin.read_text(encoding="utf-8") == expected
        if shutil.which("systemd-analyze"):
            verify_unit = root / "verify/nginx.service"
            verify_dropin = root / "verify/nginx.service.d/50-purecvisor-bind-ready.conf"
            verify_dropin.parent.mkdir(parents=True)
            verify_unit.write_text(
                "[Unit]\nDescription=fixture nginx\n"
                "[Service]\nType=oneshot\nExecStart=/bin/true\n",
                encoding="utf-8",
            )
            verify_dropin.write_text(
                expected.replace(
                    "/usr/local/libexec/purecvisor/wait-for-local-ip 192.0.2.73 60",
                    "/bin/true",
                ).replace(
                    "/usr/sbin/nginx -t -q -g 'daemon on; master_process on;'",
                    "/bin/true",
                ),
                encoding="utf-8",
            )
            subprocess.run(
                ["systemd-analyze", "verify", str(verify_unit)],
                check=True,
                capture_output=True,
                text=True,
            )
        parser = configparser.ConfigParser(interpolation=None)
        parser.read(root / "etc/purecvisor/daemon.conf")
        assert parser["tls"]["https_enabled"] == "false"
        assert parser["server"]["bind_plaintext"] == "loopback"
        assert parser["unknown"]["keep"] == "yes"
        assert parser["server"]["extra"] == "preserved"
        run(root, "--rollback", deployment_id)
        assert (root / "etc/purecvisor/daemon.conf").read_bytes() == before
        assert stat.S_IMODE((root / "etc/purecvisor/daemon.conf").stat().st_mode) == before_mode
        assert not helper.exists()
        assert not dropin.exists()
        rolledback = transaction_state(root, "rolledback", deployment_id)
        assert rolledback.is_dir()
        blocked = run(root, "--nginx-bind-ip", "192.0.2.73", check=False)
        assert blocked.returncode != 0
        assert "rollback recovery requires explicit finalization" in blocked.stderr
        run(root, "--finalize-rollback", deployment_id)
        assert not rolledback.exists()
        replacement = install_and_id(root)
        rollback_and_finalize(root, replacement)


def rollback_action_conflict_safety() -> None:
    with tempfile.TemporaryDirectory(prefix="pcv-nginx-finalize-conflict.") as directory:
        root = pathlib.Path(directory)
        seed(root)
        deployment_id = install_and_id(root)
        run(root, "--rollback", deployment_id)
        wrong = run(root, "--commit", deployment_id, check=False)
        assert wrong.returncode != 0
        assert "conflicts with requested action" in wrong.stderr
        retry = run(root, "--rollback", deployment_id, check=False)
        assert retry.returncode == 0
        wrong_finalize = run(
            root, "--finalize-rollback", "0" * 64, check=False
        )
        assert wrong_finalize.returncode != 0
        assert transaction_state(root, "rolledback", deployment_id).is_dir()
        run(root, "--finalize-rollback", deployment_id)

    with tempfile.TemporaryDirectory(prefix="pcv-nginx-duplicate-state.") as directory:
        root = pathlib.Path(directory)
        seed(root)
        deployment_id = install_and_id(root)
        pending = transaction_state(root, "pending", deployment_id)
        rolledback = transaction_state(root, "rolledback", deployment_id)
        shutil.copytree(pending, rolledback)
        rejected = run(
            root, "--finalize-rollback", deployment_id, check=False
        )
        assert rejected.returncode != 0
        assert "multiple transaction states" in rejected.stderr
        assert pending.is_dir() and rolledback.is_dir()


def commit_and_certificate_preservation() -> None:
    with tempfile.TemporaryDirectory(prefix="pcv-nginx-commit.") as directory:
        root = pathlib.Path(directory)
        seed(root)
        cert = root / "etc/purecvisor/pki/server.crt"
        key = root / "etc/purecvisor/pki/server.key"
        cert.parent.mkdir(parents=True)
        cert.write_bytes(b"certificate-unchanged")
        key.write_bytes(b"private-key-unchanged")
        deployment_id = install_and_id(root)
        run(root, "--commit", deployment_id)
        assert cert.read_bytes() == b"certificate-unchanged"
        assert key.read_bytes() == b"private-key-unchanged"
        rejected = run(root, "--rollback", deployment_id, check=False)
        assert rejected.returncode != 0


def invalid_input_and_symlink_rejection() -> None:
    for value in ("example.test", "192.0.2.73 192.0.2.74", "2001:db8::1"):
        with tempfile.TemporaryDirectory(prefix="pcv-nginx-invalid.") as directory:
            root = pathlib.Path(directory)
            seed(root)
            result = run(root, "--nginx-bind-ip", value, check=False)
            assert result.returncode != 0
    with tempfile.TemporaryDirectory(prefix="pcv-nginx-link.") as directory:
        root = pathlib.Path(directory)
        seed(root)
        outside = root / "outside"
        outside.mkdir()
        (root / "etc/systemd/system").mkdir(parents=True)
        (root / "etc/systemd/system/nginx.service.d").symlink_to(outside)
        result = run(root, "--nginx-bind-ip", "192.0.2.73", check=False)
        assert result.returncode != 0

    with tempfile.TemporaryDirectory(prefix="pcv-nginx-source-link.") as directory:
        root = pathlib.Path(directory)
        seed(root)
        source_link = root / "wait-link"
        source_link.symlink_to(WAIT_HELPER)
        result = subprocess.run(
            [str(INSTALLER), "--root", str(root), "--nginx-bind-ip", "192.0.2.73"],
            text=True,
            capture_output=True,
            env={**os.environ, "PCV_WAIT_FOR_LOCAL_IP_SOURCE": str(source_link)},
        )
        assert result.returncode != 0


def pending_recovery_and_corruption_rejection() -> None:
    with tempfile.TemporaryDirectory(prefix="pcv-nginx-pending.") as directory:
        root = pathlib.Path(directory)
        seed(root)
        first = install_and_id(root)
        first_state = root / (
            f"var/lib/purecvisor/nginx-termination-transactions/pending.{first}"
        )
        assert first_state.is_dir()
        recovered = run(root, "--nginx-bind-ip", "192.0.2.73", check=False)
        assert recovered.returncode != 0
        assert "rollback recovery requires explicit finalization" in recovered.stderr
        assert not first_state.exists()
        run(root, "--finalize-rollback", first)
        second = install_and_id(root)
        assert second != first
        assert not first_state.exists()
        run(root, "--commit", second)

    with tempfile.TemporaryDirectory(prefix="pcv-nginx-corrupt.") as directory:
        root = pathlib.Path(directory)
        seed(root)
        deployment_id = install_and_id(root)
        manifest = root / (
            f"var/lib/purecvisor/nginx-termination-transactions/"
            f"pending.{deployment_id}/manifest.json"
        )
        manifest.chmod(0o666)
        result = run(root, "--nginx-bind-ip", "192.0.2.74", check=False)
        assert result.returncode != 0


def failure_rollback_and_locking() -> None:
    with tempfile.TemporaryDirectory(prefix="pcv-nginx-failure.") as directory:
        root = pathlib.Path(directory)
        seed(root)
        before = (root / "etc/purecvisor/daemon.conf").read_bytes()
        safe_helper = root / ".fixture-wait-for-local-ip"
        safe_helper.write_bytes(WAIT_HELPER.read_bytes())
        safe_helper.chmod(0o700)
        result = subprocess.run(
            [str(INSTALLER), "--root", str(root), "--nginx-bind-ip", "192.0.2.73"],
            text=True,
            capture_output=True,
            env={
                **os.environ,
                "PCV_WAIT_FOR_LOCAL_IP_SOURCE": str(safe_helper),
                "PCV_NGINX_INSTALL_FAILPOINT": "after-write",
            },
        )
        assert result.returncode != 0
        assert (root / "etc/purecvisor/daemon.conf").read_bytes() == before
        assert not (root / "usr/local/libexec/purecvisor/wait-for-local-ip").exists()
        assert not (
            root / "etc/systemd/system/nginx.service.d/50-purecvisor-bind-ready.conf"
        ).exists()

    with tempfile.TemporaryDirectory(prefix="pcv-nginx-lock.") as directory:
        root = pathlib.Path(directory)
        seed(root)
        hold = root / "hold"
        ready = root / "ready"
        hold.touch()
        safe_helper = root / ".fixture-wait-for-local-ip"
        safe_helper.write_bytes(WAIT_HELPER.read_bytes())
        safe_helper.chmod(0o700)
        first = subprocess.Popen(
            [str(INSTALLER), "--root", str(root), "--nginx-bind-ip", "192.0.2.73"],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env={
                **os.environ,
                "PCV_WAIT_FOR_LOCAL_IP_SOURCE": str(safe_helper),
                "PCV_NGINX_TEST_HOLD_FILE": str(hold),
                "PCV_NGINX_TEST_READY_FILE": str(ready),
            },
        )
        try:
            deadline = time.monotonic() + 3
            while not ready.exists() and time.monotonic() < deadline:
                time.sleep(0.02)
            assert ready.exists()
            started = time.monotonic()
            result = run(root, "--nginx-bind-ip", "192.0.2.73", check=False)
            assert result.returncode != 0
            assert time.monotonic() - started < 3
        finally:
            hold.unlink(missing_ok=True)
            first.wait(timeout=3)
        assert first.returncode == 0


def sigkill_pending_and_source_race() -> None:
    with tempfile.TemporaryDirectory(prefix="pcv-nginx-kill.") as directory:
        root = pathlib.Path(directory)
        seed(root)
        before = (root / "etc/purecvisor/daemon.conf").read_bytes()
        hold = root / "hold"
        ready = root / "ready"
        hold.touch()
        safe_helper = root / ".fixture-wait-for-local-ip"
        safe_helper.write_bytes(WAIT_HELPER.read_bytes())
        safe_helper.chmod(0o700)
        process = subprocess.Popen(
            [str(INSTALLER), "--root", str(root), "--nginx-bind-ip", "192.0.2.73"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env={
                **os.environ,
                "PCV_WAIT_FOR_LOCAL_IP_SOURCE": str(safe_helper),
                "PCV_NGINX_TEST_HOLD_FILE": str(hold),
                "PCV_NGINX_TEST_READY_FILE": str(ready),
            },
        )
        deadline = time.monotonic() + 3
        while not ready.exists() and time.monotonic() < deadline:
            time.sleep(0.02)
        assert ready.exists()
        process.kill()
        process.wait(timeout=3)
        hold.unlink()
        recovered = run(root, "--nginx-bind-ip", "192.0.2.73", check=False)
        assert recovered.returncode != 0
        rolledback = list(
            (root / "var/lib/purecvisor/nginx-termination-transactions").glob(
                "rolledback.*"
            )
        )
        assert len(rolledback) == 1
        run(root, "--finalize-rollback", rolledback[0].name.split(".", 1)[1])
        second = install_and_id(root)
        rollback_and_finalize(root, second)
        assert (root / "etc/purecvisor/daemon.conf").read_bytes() == before

    with tempfile.TemporaryDirectory(prefix="pcv-nginx-source-race.") as directory:
        root = pathlib.Path(directory)
        seed(root)
        source = root / "wait-helper"
        source.write_bytes(WAIT_HELPER.read_bytes())
        source.chmod(0o700)
        hold = root / "source-hold"
        ready = root / "source-ready"
        hold.touch()
        process = subprocess.Popen(
            [str(INSTALLER), "--root", str(root), "--nginx-bind-ip", "192.0.2.73"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env={
                **os.environ,
                "PCV_WAIT_FOR_LOCAL_IP_SOURCE": str(source),
                "PCV_NGINX_TEST_SOURCE_HOLD_FILE": str(hold),
                "PCV_NGINX_TEST_SOURCE_READY_FILE": str(ready),
            },
        )
        deadline = time.monotonic() + 3
        while not ready.exists() and time.monotonic() < deadline:
            time.sleep(0.02)
        assert ready.exists()
        replacement = root / "replacement"
        replacement.write_bytes(b"#!/bin/sh\nexit 99\n")
        replacement.chmod(0o755)
        os.replace(replacement, source)
        hold.unlink()
        _stdout, _stderr = process.communicate(timeout=3)
        assert process.returncode != 0
        assert not (root / "usr/local/libexec/purecvisor/wait-for-local-ip").exists()


def sudo_uid_source_contract() -> None:
    with tempfile.TemporaryDirectory(prefix="pcv-nginx-sudo-owner.") as directory:
        root = pathlib.Path(directory)
        seed(root)
        source = root / "trusted-stage/wait-for-local-ip.sh"
        source.parent.mkdir(mode=0o700)
        source.write_bytes(WAIT_HELPER.read_bytes())
        source.chmod(0o700)
        simulated = {
            **os.environ,
            "PCV_WAIT_FOR_LOCAL_IP_SOURCE": str(source),
            "PCV_NGINX_TEST_EFFECTIVE_UID": "0",
            "PCV_NGINX_TEST_SUDO_UID": str(os.getuid()),
        }
        accepted = subprocess.run(
            [str(INSTALLER), "--root", str(root), "--nginx-bind-ip", "192.0.2.73"],
            text=True,
            capture_output=True,
            env=simulated,
        )
        assert accepted.returncode == 0, accepted.stderr
        deployment_id = accepted.stdout.strip().split("=", 1)[1]
        rollback_and_finalize(root, deployment_id)

        rejected = subprocess.run(
            [str(INSTALLER), "--root", str(root), "--nginx-bind-ip", "192.0.2.73"],
            text=True,
            capture_output=True,
            env={
                **simulated,
                "PCV_NGINX_TEST_SUDO_UID": str(os.getuid() + 1),
            },
        )
        assert rejected.returncode != 0

        source.parent.chmod(0o755)
        unsafe_parent = subprocess.run(
            [str(INSTALLER), "--root", str(root), "--nginx-bind-ip", "192.0.2.73"],
            text=True,
            capture_output=True,
            env=simulated,
        )
        assert unsafe_parent.returncode != 0


def kill_at_point(
    root: pathlib.Path,
    point: str,
    arguments: list[str],
    safe_helper: pathlib.Path,
) -> None:
    hold = root / f"hold-{point}"
    ready = root / f"ready-{point}"
    hold.touch()
    process = subprocess.Popen(
        [str(INSTALLER), "--root", str(root), *arguments],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env={
            **os.environ,
            "PCV_WAIT_FOR_LOCAL_IP_SOURCE": str(safe_helper),
            "PCV_NGINX_TEST_CRASH_POINT": point,
            "PCV_NGINX_TEST_CRASH_HOLD_FILE": str(hold),
            "PCV_NGINX_TEST_CRASH_READY_FILE": str(ready),
        },
    )
    deadline = time.monotonic() + 3
    while not ready.exists() and time.monotonic() < deadline:
        time.sleep(0.01)
    assert ready.exists(), (point, process.poll(), process.stderr.read() if process.poll() else "")
    process.kill()
    process.wait(timeout=3)
    hold.unlink(missing_ok=True)


def transaction_state_crash_consistency() -> None:
    with tempfile.TemporaryDirectory(prefix="pcv-nginx-partial-write.") as directory:
        root = pathlib.Path(directory)
        seed(root)
        state = root / "var/lib/purecvisor/nginx-termination-transactions"
        state.mkdir(parents=True, mode=0o700)
        partial = state / f"creating.{'a' * 64}"
        partial.mkdir(mode=0o700)
        temporary = partial / f".manifest.json.pcv-{'b' * 32}"
        temporary.write_bytes(b'{"partial":')
        temporary.chmod(0o600)
        replacement = install_and_id(root)
        rollback_and_finalize(root, replacement)
        assert not partial.exists()

    for point in ("creating-directory", "creating-backups", "pending-before-mutation"):
        with tempfile.TemporaryDirectory(prefix=f"pcv-nginx-{point}.") as directory:
            root = pathlib.Path(directory)
            seed(root)
            before = (root / "etc/purecvisor/daemon.conf").read_bytes()
            safe_helper = root / ".fixture-wait-for-local-ip"
            safe_helper.write_bytes(WAIT_HELPER.read_bytes())
            safe_helper.chmod(0o700)
            kill_at_point(
                root,
                point,
                ["--nginx-bind-ip", "192.0.2.73"],
                safe_helper,
            )
            assert (root / "etc/purecvisor/daemon.conf").read_bytes() == before
            if point == "pending-before-mutation":
                recovered = run(root, "--nginx-bind-ip", "192.0.2.73", check=False)
                assert recovered.returncode != 0
                rolledback = list(
                    (
                        root
                        / "var/lib/purecvisor/nginx-termination-transactions"
                    ).glob("rolledback.*")
                )
                assert len(rolledback) == 1
                run(
                    root,
                    "--finalize-rollback",
                    rolledback[0].name.split(".", 1)[1],
                )
            replacement = install_and_id(root)
            rollback_and_finalize(root, replacement)
            assert (root / "etc/purecvisor/daemon.conf").read_bytes() == before

    for point in ("commit-tombstone", "cleanup-after-unlink"):
        with tempfile.TemporaryDirectory(prefix=f"pcv-nginx-{point}.") as directory:
            root = pathlib.Path(directory)
            seed(root)
            safe_helper = root / ".fixture-wait-for-local-ip"
            safe_helper.write_bytes(WAIT_HELPER.read_bytes())
            safe_helper.chmod(0o700)
            committed = install_and_id(root)
            committed_config = (root / "etc/purecvisor/daemon.conf").read_bytes()
            kill_at_point(root, point, ["--commit", committed], safe_helper)
            replacement = install_and_id(root)
            rollback_and_finalize(root, replacement)
            assert (root / "etc/purecvisor/daemon.conf").read_bytes() == committed_config

    with tempfile.TemporaryDirectory(prefix="pcv-nginx-cross-commit.") as directory:
        root = pathlib.Path(directory)
        seed(root)
        safe_helper = root / ".fixture-wait-for-local-ip"
        safe_helper.write_bytes(WAIT_HELPER.read_bytes())
        safe_helper.chmod(0o700)
        deployment_id = install_and_id(root)
        kill_at_point(root, "commit-tombstone", ["--commit", deployment_id], safe_helper)
        wrong = run(root, "--rollback", deployment_id, check=False)
        assert wrong.returncode != 0
        assert "conflicts with requested action" in wrong.stderr
        assert run(root, "--commit", deployment_id, check=False).returncode == 0

    for point in ("rollback-restored", "rollback-tombstone"):
        with tempfile.TemporaryDirectory(prefix=f"pcv-nginx-{point}.") as directory:
            root = pathlib.Path(directory)
            seed(root)
            before = (root / "etc/purecvisor/daemon.conf").read_bytes()
            safe_helper = root / ".fixture-wait-for-local-ip"
            safe_helper.write_bytes(WAIT_HELPER.read_bytes())
            safe_helper.chmod(0o700)
            deployed = install_and_id(root)
            kill_at_point(root, point, ["--rollback", deployed], safe_helper)
            assert run(root, "--rollback", deployed, check=False).returncode == 0
            run(root, "--finalize-rollback", deployed)
            replacement = install_and_id(root)
            rollback_and_finalize(root, replacement)
            assert (root / "etc/purecvisor/daemon.conf").read_bytes() == before

    with tempfile.TemporaryDirectory(prefix="pcv-nginx-cross-rollback.") as directory:
        root = pathlib.Path(directory)
        seed(root)
        safe_helper = root / ".fixture-wait-for-local-ip"
        safe_helper.write_bytes(WAIT_HELPER.read_bytes())
        safe_helper.chmod(0o700)
        deployment_id = install_and_id(root)
        kill_at_point(root, "rollback-tombstone", ["--rollback", deployment_id], safe_helper)
        wrong = run(root, "--commit", deployment_id, check=False)
        assert wrong.returncode != 0
        assert "conflicts with requested action" in wrong.stderr
        assert run(root, "--rollback", deployment_id, check=False).returncode == 0
        run(root, "--finalize-rollback", deployment_id)

    for point in ("transaction-tombstone", "cleanup-after-unlink"):
        with tempfile.TemporaryDirectory(prefix=f"pcv-nginx-io-{point}.") as directory:
            root = pathlib.Path(directory)
            seed(root)
            deployment_id = install_and_id(root)
            committed_config = (root / "etc/purecvisor/daemon.conf").read_bytes()
            failed = subprocess.run(
                [str(INSTALLER), "--root", str(root), "--commit", deployment_id],
                text=True,
                capture_output=True,
                env={
                    **os.environ,
                    "PCV_NGINX_TEST_ERROR_POINT": point,
                },
            )
            assert failed.returncode != 0
            wrong = run(root, "--rollback", deployment_id, check=False)
            assert wrong.returncode != 0
            resumed = run(root, "--commit", deployment_id, check=False)
            assert resumed.returncode == 0
            replacement = install_and_id(root)
            rollback_and_finalize(root, replacement)
            assert (root / "etc/purecvisor/daemon.conf").read_bytes() == committed_config

    with tempfile.TemporaryDirectory(prefix="pcv-nginx-rollback-io.") as directory:
        root = pathlib.Path(directory)
        seed(root)
        deployment_id = install_and_id(root)
        failed = subprocess.run(
            [str(INSTALLER), "--root", str(root), "--rollback", deployment_id],
            text=True,
            capture_output=True,
            env={
                **os.environ,
                "PCV_NGINX_TEST_ERROR_POINT": "transaction-tombstone",
            },
        )
        assert failed.returncode != 0
        wrong = run(root, "--commit", deployment_id, check=False)
        assert wrong.returncode != 0
        assert run(root, "--rollback", deployment_id, check=False).returncode == 0
        run(root, "--finalize-rollback", deployment_id)


def metadata_and_action_contract() -> None:
    installer_source = INSTALLER.read_text(encoding="utf-8")
    assert '["systemctl", "daemon-reload"]' in installer_source
    assert "reload_timeout = 10.0" in installer_source
    with tempfile.TemporaryDirectory(prefix="pcv-nginx-metadata.") as directory:
        root = pathlib.Path(directory)
        seed(root)
        daemon = root / "etc/purecvisor/daemon.conf"
        alternate_groups = [group for group in os.getgroups() if group != os.getgid()]
        if alternate_groups:
            os.chown(daemon, os.getuid(), alternate_groups[0])
        daemon.chmod(0o4640)
        before = daemon.stat()
        deployment_id = install_and_id(root)
        rollback_and_finalize(root, deployment_id)
        after = daemon.stat()
        assert stat.S_IMODE(after.st_mode) == stat.S_IMODE(before.st_mode)
        assert (after.st_uid, after.st_gid) == (before.st_uid, before.st_gid)

        conflicting = subprocess.run(
            [
                str(INSTALLER),
                "--root",
                str(root),
                "--rollback",
                "0" * 64,
                "--commit",
                "1" * 64,
            ],
            text=True,
            capture_output=True,
        )
        assert conflicting.returncode == 2

    with tempfile.TemporaryDirectory(prefix="pcv-nginx-bool-manifest.") as directory:
        root = pathlib.Path(directory)
        seed(root)
        deployment_id = install_and_id(root)
        manifest = root / (
            "var/lib/purecvisor/nginx-termination-transactions/"
            f"pending.{deployment_id}/manifest.json"
        )
        document = json.loads(manifest.read_text(encoding="utf-8"))
        document["targets"]["daemon"]["uid"] = True
        manifest.write_text(json.dumps(document), encoding="utf-8")
        manifest.chmod(0o600)
        rejected = run(root, "--rollback", deployment_id, check=False)
        assert rejected.returncode != 0


def daemon_reload_timeout_and_identity_seam() -> None:
    with tempfile.TemporaryDirectory(prefix="pcv-nginx-reload-timeout.") as directory:
        root = pathlib.Path(directory)
        seed(root)
        before = (root / "etc/purecvisor/daemon.conf").read_bytes()
        source = root / ".fixture-wait-for-local-ip"
        source.write_bytes(WAIT_HELPER.read_bytes())
        source.chmod(0o700)
        sleeper = root / "reload-sleeper"
        sleeper.write_text("#!/bin/sh\nexec sleep 10\n", encoding="utf-8")
        sleeper.chmod(0o700)
        started = time.monotonic()
        result = subprocess.run(
            [str(INSTALLER), "--root", str(root), "--nginx-bind-ip", "192.0.2.73"],
            text=True,
            capture_output=True,
            env={
                **os.environ,
                "PCV_WAIT_FOR_LOCAL_IP_SOURCE": str(source),
                "PCV_NGINX_TEST_DAEMON_RELOAD_COMMAND": str(sleeper),
                "PCV_NGINX_TEST_DAEMON_RELOAD_TIMEOUT": "0.1",
            },
        )
        assert result.returncode != 0
        assert time.monotonic() - started < 2
        assert (root / "etc/purecvisor/daemon.conf").read_bytes() == before
        assert "systemd daemon-reload failed; rollback state preserved" in result.stderr
        states = list(
            (root / "var/lib/purecvisor/nginx-termination-transactions").glob(
                "rolledback.*"
            )
        )
        assert len(states) == 1
        deployment_id = states[0].name.split(".", 1)[1]
        retry = run(root, "--rollback", deployment_id, check=False)
        assert retry.returncode == 0
        run(root, "--finalize-rollback", deployment_id)

    with tempfile.TemporaryDirectory(prefix="pcv-nginx-identity-seam.") as directory:
        root = pathlib.Path(directory)
        seed(root)
        source = root / ".fixture-wait-for-local-ip"
        source.write_bytes(WAIT_HELPER.read_bytes())
        source.chmod(0o700)
        identity_log = root / "identity.log"
        installed = subprocess.run(
            [str(INSTALLER), "--root", str(root), "--nginx-bind-ip", "192.0.2.73"],
            text=True,
            capture_output=True,
            check=True,
            env={
                **os.environ,
                "PCV_WAIT_FOR_LOCAL_IP_SOURCE": str(source),
                "PCV_NGINX_TEST_CAPTURE_UID": "424242",
                "PCV_NGINX_TEST_CAPTURE_GID": "434343",
                "PCV_NGINX_TEST_FCHOWN_LOG": str(identity_log),
            },
        )
        deployment_id = installed.stdout.strip().split("=", 1)[1]
        rolled_back = subprocess.run(
            [str(INSTALLER), "--root", str(root), "--rollback", deployment_id],
            text=True,
            capture_output=True,
            env={
                **os.environ,
                "PCV_NGINX_TEST_FCHOWN_LOG": str(identity_log),
            },
        )
        assert rolled_back.returncode == 0
        assert "424242:434343" in identity_log.read_text(encoding="ascii")


SNIPPET = "etc/nginx/snippets/purecvisor-proxy-auth.conf"

def proxy_auth_secret_contract() -> None:

    with tempfile.TemporaryDirectory(prefix="pcv-nginx-proxyauth.") as directory:
        root = pathlib.Path(directory)
        seed(root)
        before = (root / "etc/purecvisor/daemon.conf").read_bytes()
        first = install_and_id(root)
        snippet = root / SNIPPET
        assert_regular(snippet, 0o600)
        body = snippet.read_text(encoding="utf-8")
        match = re.search(r'^proxy_set_header X-PCV-Proxy-Auth "([0-9a-f]{64})";$', body, re.M)
        assert match, body
        secret = match.group(1)
        parser = configparser.ConfigParser(interpolation=None)
        parser.read(root / "etc/purecvisor/daemon.conf")
        assert parser["server"]["proxy_auth_secret"] == secret
        result = run(root, "--commit", first)
        assert secret not in result.stdout + result.stderr
        second = install_and_id(root)
        assert secret in snippet.read_text(encoding="utf-8")
        parser = configparser.ConfigParser(interpolation=None)
        parser.read(root / "etc/purecvisor/daemon.conf")
        assert parser["server"]["proxy_auth_secret"] == secret
        rollback_and_finalize(root, second)
        assert secret in snippet.read_text(encoding="utf-8")
        assert (root / "etc/purecvisor/daemon.conf").read_bytes() != before
    with tempfile.TemporaryDirectory(prefix="pcv-nginx-proxyauth-rb.") as directory:
        root = pathlib.Path(directory)
        seed(root)
        before = (root / "etc/purecvisor/daemon.conf").read_bytes()
        deployment_id = install_and_id(root)
        run(root, "--rollback", deployment_id)
        assert (root / "etc/purecvisor/daemon.conf").read_bytes() == before
        assert not (root / SNIPPET).exists()
        run(root, "--finalize-rollback", deployment_id)
    for bad in ("ENC2:QUJDREVGR0hJSktMTU5PUFFSU1RVVldYWVo=", 'x' * 32 + '"; return 444; #', "short-secret"):
        with tempfile.TemporaryDirectory(prefix="pcv-nginx-proxyauth-bad.") as directory:
            root = pathlib.Path(directory)
            seed(root)
            conf = root / "etc/purecvisor/daemon.conf"
            conf.write_text(conf.read_text(encoding="utf-8") + f"proxy_auth_secret={bad}\n", encoding="utf-8")
            before = conf.read_bytes()
            result = run(root, "--nginx-bind-ip", "10.168.3.73", check=False)
            assert result.returncode == 1, result
            assert "proxy_auth_secret" in result.stderr, result.stderr
            assert bad not in result.stderr
            assert conf.read_bytes() == before
            assert not (root / SNIPPET).exists()
    proxy_auth_secret_daemon_parity()
    proxy_auth_secret_self_check_contract()

GLIB_SPACE = " \t\n\f\r"

SECRET_A = secrets.token_hex(32)
SECRET_B = secrets.token_hex(32)
CONF_HEAD = "[unknown]\nkeep=yes\n\n[tls]\nhttps_enabled=true\n\n"

def gkeyfile_model(data: bytes) -> dict[tuple[str, str], str] | None:

    values: dict[tuple[str, str], str] = {}
    group = None
    parts = data.decode("utf-8").split("\n")
    for index, line in enumerate(parts):
        if index < len(parts) - 1 and line.endswith("\r"):
            line = line[:-1]
        text = line.lstrip(GLIB_SPACE)
        if not text or text.startswith("#"):
            continue
        if text.startswith("["):
            header = re.fullmatch(r"\[([^\]]*)\][ \t]*", text)
            if not header:
                return None
            name = header.group(1)
            if not name or "[" in name or any(ord(ch) < 32 or ord(ch) == 127 for ch in name):
                return None
            group = name
            continue
        if "=" not in text or text.startswith("=") or group is None:
            return None
        key, raw = text.split("=", 1)
        values[(group, key.rstrip(GLIB_SPACE))] = raw.lstrip(GLIB_SPACE)
    return values

def daemon_effective_secret(data: bytes) -> str | None:

    model = gkeyfile_model(data)
    return (model or {}).get(("server", "proxy_auth_secret")) or None

def proxy_auth_secret_daemon_parity() -> None:

    rejects = {

        "daemon-and-server": f"[daemon]\nproxy_auth_secret={SECRET_A}\n\n"
                             f"[server]\nbind_plaintext=all\nproxy_auth_secret={SECRET_B}\n",
        "duplicate-key": f"[server]\nproxy_auth_secret={SECRET_A}\nproxy_auth_secret={SECRET_B}\n",
        "duplicate-groups": f"[server]\nproxy_auth_secret={SECRET_A}\n\n[server]\nproxy_auth_secret={SECRET_B}\n",
        "trailing-spaces": f"[server]\nproxy_auth_secret={SECRET_A}   \n",
        "trailing-tab": f"[server]\nproxy_auth_secret={SECRET_A}\t\n",
        "duplicate-groups-empty-first": f"[server]\nproxy_auth_secret=\n\n[server]\nproxy_auth_secret={SECRET_A}\n",
        "form-feed": f"[server]\nproxy_auth_secret={SECRET_A}\x0c{SECRET_B}\n",

        "daemon-only": f"[daemon]\nproxy_auth_secret={SECRET_A}\n\n[server]\nbind_plaintext=all\n",
        "spaced-header": f"[ server ]\nproxy_auth_secret={SECRET_A}\n",
        "duplicate-groups-empty-second": "[server]\nbind_plaintext=all\n\n[server]\nproxy_auth_secret=\n",
        "padded-header": f"[server] \nproxy_auth_secret={SECRET_A}\n",

        "inline-comment": f"[server]\nproxy_auth_secret={SECRET_A} # note\n",
        "quoted": f'[server]\nproxy_auth_secret="{SECRET_A}"\n',
        "enc2-daemon-valid-server": "[daemon]\nproxy_auth_secret=ENC2:QUJDREVGR0hJSktMTU5PUFFSU1RVVldYWVo=\n\n"
                                    f"[server]\nproxy_auth_secret={SECRET_A}\n",

        "cr-at-eof": f"[server]\nbind_plaintext=all\nproxy_auth_secret={SECRET_A}\r",

        "line-without-equals": f"[server]\nbind_plaintext=all\ngarbage\nproxy_auth_secret={SECRET_A}\n",
        "equals-first": "[server]\nbind_plaintext=all\n=value\n",
        "bracket-line-not-group": "[server]\nbind_plaintext=all\n[foo] x\n",
        "bracket-key": "[server]\nbind_plaintext=all\n[foo]=bar\n",
        "invalid-group-name": "[server]\nbind_plaintext=all\n[a[b]\nx=1\n",
        "empty-group-name": "[server]\nbind_plaintext=all\n[]\nx=1\n",
        "vt-only-line": "[server]\nbind_plaintext=all\n\v\n",
        "semicolon-comment": "[server]\nbind_plaintext=all\n; note\n",
    }
    unloadable = {"line-without-equals", "equals-first", "bracket-line-not-group", "bracket-key", "invalid-group-name",
                  "empty-group-name", "vt-only-line", "semicolon-comment", "key-before-first-group", "utf8-bom"}
    raw_rejects = {
        "key-before-first-group": "stray=1\n" + CONF_HEAD + f"[server]\nproxy_auth_secret={SECRET_A}\n",
        "utf8-bom": "\ufeff" + CONF_HEAD + f"[server]\nproxy_auth_secret={SECRET_A}\n",
    }
    for name, body in [*((n, CONF_HEAD + b) for n, b in rejects.items()), *raw_rejects.items()]:
        with tempfile.TemporaryDirectory(prefix="pcv-nginx-proxyauth-amb.") as directory:
            root = pathlib.Path(directory)
            seed(root)
            conf = root / "etc/purecvisor/daemon.conf"
            conf.write_bytes(body.encode("utf-8"))
            assert (gkeyfile_model(conf.read_bytes()) is None) == (name in unloadable), name
            before = conf.read_bytes()
            result = run(root, "--nginx-bind-ip", "10.168.3.73", check=False)
            assert result.returncode == 1, (name, result)
            assert "proxy_auth_secret" in result.stderr, (name, result.stderr)
            output = result.stdout + result.stderr
            assert SECRET_A not in output and SECRET_B not in output, name
            assert conf.read_bytes() == before, name
            assert not (root / SNIPPET).exists(), name
    matches = {
        "crlf": ((CONF_HEAD + f"[server]\nbind_plaintext=all\nproxy_auth_secret={SECRET_A}\n").replace("\n", "\r\n"),
                 SECRET_A),
        "spaces-around-equals": (f"[server]\nproxy_auth_secret =   {SECRET_A}\n", SECRET_A),
        "empty-single-server": ("[server]\nbind_plaintext=all\nproxy_auth_secret=\n", None),
        "no-key": ("[server]\nbind_plaintext=all\n", None),
        "no-server-group": ("", None),

        "no-eol-secret-last": (f"[server]\nproxy_auth_secret={SECRET_A}", SECRET_A),
        "no-eol-other-key-last": ("[server]\nextra=1", None),

        "vt-before-key": (f"[server]\nbind_plaintext=all\n\vproxy_auth_secret={SECRET_A}\n", None),
        "vt-before-equals": (f"[server]\nbind_plaintext=all\nproxy_auth_secret\v={SECRET_A}\n", None),
    }
    for name, (body, expected) in matches.items():
        with tempfile.TemporaryDirectory(prefix="pcv-nginx-proxyauth-match.") as directory:
            root = pathlib.Path(directory)
            seed(root)
            conf = root / "etc/purecvisor/daemon.conf"
            conf.write_bytes((body if name == "crlf" else CONF_HEAD + body).encode("utf-8"))
            result = run(root, "--nginx-bind-ip", "10.168.3.73", check=False)
            assert result.returncode == 0, (name, result)
            snippet = (root / SNIPPET).read_text(encoding="utf-8")
            found = re.search(r'^proxy_set_header X-PCV-Proxy-Auth "([^"]+)";$', snippet, re.M)
            assert found, (name, snippet)
            secret = found.group(1)
            if expected is not None:
                assert secret == expected, name
            else:
                assert re.fullmatch(r"[0-9a-f]{64}", secret), name
            written = gkeyfile_model(conf.read_bytes())
            assert written is not None, name
            assert written.get(("server", "proxy_auth_secret")) == secret, name
            assert written.get(("server", "bind_plaintext")) == "loopback", name
            assert written.get(("tls", "https_enabled")) == "false", name
            if name == "no-eol-other-key-last":
                assert written.get(("server", "extra")) == "1", name
            assert secret not in result.stdout + result.stderr, name

def proxy_auth_secret_self_check_contract() -> None:

    source = INSTALLER.read_text(encoding="utf-8")
    target = 'lines = set_key(lines, "server", "proxy_auth_secret", secret_to_write)'
    assert source.count(target) == 1, "installer secret write line changed — update this mutation"
    with tempfile.TemporaryDirectory(prefix="pcv-nginx-selfcheck.") as directory:
        base = pathlib.Path(directory)
        mutant = base / "install-nginx-termination-mutant.sh"
        mutant.write_text(source.replace(target, target.replace('"server"', '"server-elsewhere"')), encoding="utf-8")
        mutant.chmod(0o700)
        root = base / "root"
        root.mkdir(mode=0o700)
        seed(root)
        conf = root / "etc/purecvisor/daemon.conf"
        before = conf.read_bytes()
        result = run(root, "--nginx-bind-ip", "10.168.3.73", check=False, installer=mutant)
        assert result.returncode == 1, result
        assert "internal check failed" in result.stderr, result.stderr
        assert "PCV_NGINX_DEPLOYMENT_ID" not in result.stdout, result.stdout
        assert not re.search(r"[0-9a-f]{64}", result.stdout + result.stderr)
        assert conf.read_bytes() == before
        assert not (root / SNIPPET).exists()

def legacy_pending_manifest_rollback() -> None:

    with tempfile.TemporaryDirectory(prefix="pcv-nginx-legacy.") as directory:
        root = pathlib.Path(directory)
        seed(root)
        before = (root / "etc/purecvisor/daemon.conf").read_bytes()
        deployment_id = install_and_id(root)
        tx = transaction_state(root, "pending", deployment_id)
        manifest_path = tx / "manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest["targets"].pop("proxyauth")
        manifest_path.write_text(json.dumps(manifest, sort_keys=True, separators=(",", ":")), encoding="utf-8")
        manifest_path.chmod(0o600)
        backup = tx / "proxyauth.bak"
        if backup.exists():
            backup.unlink()
        run(root, "--rollback", deployment_id)
        assert (root / "etc/purecvisor/daemon.conf").read_bytes() == before
        run(root, "--finalize-rollback", deployment_id)

def rollback_daemon_reload_contract() -> None:

    for failing in (False, True):
        with tempfile.TemporaryDirectory(prefix="pcv-nginx-rb-reload.") as directory:
            root = pathlib.Path(directory)
            seed(root)
            before = (root / "etc/purecvisor/daemon.conf").read_bytes()
            dropin = root / "etc/systemd/system/nginx.service.d/50-purecvisor-bind-ready.conf"
            log = root / "reload.log"
            hook = root / "reload-hook"

            hook.write_text(
                f'#!/bin/sh\nif [ -e "{dropin}" ]; then echo present >> "{log}"; '
                f'else echo absent >> "{log}"; fi\n'
                + f'[ ! -e "{root}/reload-fail" ]\n',
                encoding="utf-8",
            )
            hook.chmod(0o700)
            source = root / ".fixture-wait-for-local-ip"
            source.write_bytes(WAIT_HELPER.read_bytes())
            source.chmod(0o700)
            env = {
                **os.environ,
                "PCV_WAIT_FOR_LOCAL_IP_SOURCE": str(source),
                "PCV_NGINX_TEST_DAEMON_RELOAD_COMMAND": str(hook),
            }
            installed = subprocess.run(
                [str(INSTALLER), "--root", str(root), "--nginx-bind-ip", "10.168.3.73"],
                text=True, capture_output=True, check=True, env=env,
            )
            deployment_id = installed.stdout.strip().split("=", 1)[1]
            assert log.read_text(encoding="ascii").splitlines() == ["present"]
            if failing:
                (root / "reload-fail").write_text("", encoding="ascii")
            rolled_back = subprocess.run(
                [str(INSTALLER), "--root", str(root), "--rollback", deployment_id],
                text=True, capture_output=True, env=env,
            )
            calls = log.read_text(encoding="ascii").splitlines()
            assert calls == ["present", "absent"], calls
            assert (root / "etc/purecvisor/daemon.conf").read_bytes() == before
            assert not dropin.exists()
            assert transaction_state(root, "rolledback", deployment_id).is_dir()
            if failing:
                assert rolled_back.returncode == 1, rolled_back
                assert "systemd daemon-reload failed after rollback" in rolled_back.stderr
                assert "systemctl daemon-reload" in rolled_back.stderr
            else:
                assert rolled_back.returncode == 0, rolled_back
            run(root, "--finalize-rollback", deployment_id)

def pending_restore_daemon_reload_contract() -> None:

    for failing in (False, True):
        with tempfile.TemporaryDirectory(prefix="pcv-nginx-pending-reload.") as directory:
            root = pathlib.Path(directory)
            seed(root)
            before = (root / "etc/purecvisor/daemon.conf").read_bytes()
            dropin = root / "etc/systemd/system/nginx.service.d/50-purecvisor-bind-ready.conf"
            log = root / "reload.log"
            hook = root / "reload-hook"
            hook.write_text(
                f'#!/bin/sh\nif [ -e "{dropin}" ]; then echo present >> "{log}"; '
                f'else echo absent >> "{log}"; fi\n'
                + f'[ ! -e "{root}/reload-fail" ]\n',
                encoding="utf-8",
            )
            hook.chmod(0o700)
            source = root / ".fixture-wait-for-local-ip"
            source.write_bytes(WAIT_HELPER.read_bytes())
            source.chmod(0o700)
            env = {
                **os.environ,
                "PCV_WAIT_FOR_LOCAL_IP_SOURCE": str(source),
                "PCV_NGINX_TEST_DAEMON_RELOAD_COMMAND": str(hook),
            }
            command = [str(INSTALLER), "--root", str(root), "--nginx-bind-ip", "10.168.3.73"]
            first = subprocess.run(command, text=True, capture_output=True, check=True, env=env)
            first_id = first.stdout.strip().split("=", 1)[1]
            assert transaction_state(root, "pending", first_id).is_dir()
            assert log.read_text(encoding="ascii").splitlines() == ["present"]
            if failing:
                (root / "reload-fail").write_text("", encoding="ascii")
            again = subprocess.run(command, text=True, capture_output=True, env=env)
            assert again.returncode == 1, again
            assert "pending nginx transaction rolled back" in again.stderr, again.stderr
            assert "rollback recovery requires explicit finalization" in again.stderr, again.stderr
            calls = log.read_text(encoding="ascii").splitlines()
            assert calls == ["present", "absent"], calls
            assert (root / "etc/purecvisor/daemon.conf").read_bytes() == before
            assert not dropin.exists()
            assert not (root / SNIPPET).exists()
            assert not transaction_state(root, "pending", first_id).exists()
            assert transaction_state(root, "rolledback", first_id).is_dir()
            assert not re.search(r"PCV_NGINX_DEPLOYMENT_ID=", again.stdout)
            if failing:
                assert "systemd daemon-reload failed" in again.stderr, again.stderr
                assert "systemctl daemon-reload" in again.stderr
            else:
                assert "daemon-reload failed" not in again.stderr, again.stderr
            run(root, "--finalize-rollback", first_id)
            assert not transaction_state(root, "rolledback", first_id).exists()
            assert log.read_text(encoding="ascii").splitlines() == ["present", "absent"]

def main() -> None:
    basic_install_and_rollback()
    rollback_action_conflict_safety()
    commit_and_certificate_preservation()
    invalid_input_and_symlink_rejection()
    pending_recovery_and_corruption_rejection()
    failure_rollback_and_locking()
    sigkill_pending_and_source_race()
    sudo_uid_source_contract()
    transaction_state_crash_consistency()
    metadata_and_action_contract()
    daemon_reload_timeout_and_identity_seam()
    proxy_auth_secret_contract()
    legacy_pending_manifest_rollback()
    rollback_daemon_reload_contract()
    pending_restore_daemon_reload_contract()
    print("nginx-termination-security-ok")


if __name__ == "__main__":
    main()
