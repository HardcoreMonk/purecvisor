#!/usr/bin/env python3












from __future__ import annotations

import os
from pathlib import Path
import stat
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
HELPER = ROOT / "scripts" / "check-deploy-abi.sh"


class DeployAbiPreflightTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="pcv-abi-test-")
        self.root = Path(self.temp.name)
        self.fake_bin = self.root / "bin"
        self.fake_bin.mkdir()
        self.daemon = self.root / "purecvisorsd"
        self.cli = self.root / "pcvctl"
        for artifact in (self.daemon, self.cli):
            artifact.write_bytes(b"trusted-fixture-elf\n")
            artifact.chmod(0o755)

        ldd = self.fake_bin / "ldd"
        ldd.write_text(
            """#!/bin/sh
case "${FAKE_LDD_MODE:-ok}" in
  ok)
    printf '%s\\n' 'libc.so.6 => /lib/libc.so.6 (0x1)'
    exit 0
    ;;
  missing)
    if [ "${1##*/}" = purecvisorsd ]; then
      printf '%s\\n' 'libxml2.so.2 => not found'
    else
      printf '%s\\n' 'libc.so.6 => /lib/libc.so.6 (0x1)'
    fi
    exit 0
    ;;
  loader-fail)
    printf '%s\\n' 'not a dynamic executable' >&2
    exit 1
    ;;
  *) exit 2 ;;
esac
""",
            encoding="utf-8",
        )
        ldd.chmod(ldd.stat().st_mode | stat.S_IXUSR)

    def tearDown(self) -> None:
        self.temp.cleanup()

    def run_helper(
        self, *artifacts: Path, mode: str = "ok", path: str | None = None
    ) -> subprocess.CompletedProcess[str]:
        env = os.environ.copy()
        env["PATH"] = path if path is not None else str(self.fake_bin)
        env["FAKE_LDD_MODE"] = mode
        return subprocess.run(
            ["/bin/bash", str(HELPER), *(str(item) for item in artifacts)],
            check=False,
            text=True,
            capture_output=True,
            env=env,
        )

    def test_two_artifacts_pass_with_exact_marker(self) -> None:
        result = self.run_helper(self.daemon, self.cli)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            result.stdout, "PCV_DEPLOY_ABI_PREFLIGHT=OK binaries=2\n"
        )

    def test_not_found_fails_even_when_ldd_returns_zero(self) -> None:
        result = self.run_helper(self.daemon, self.cli, mode="missing")
        self.assertEqual(result.returncode, 1)
        self.assertIn("libxml2.so.2 => not found", result.stderr)
        self.assertNotIn("PCV_DEPLOY_ABI_PREFLIGHT=OK", result.stdout)

    def test_loader_nonzero_fails(self) -> None:
        result = self.run_helper(self.daemon, self.cli, mode="loader-fail")
        self.assertEqual(result.returncode, 1)
        self.assertIn("not a dynamic executable", result.stderr)

    def test_missing_ldd_fails_closed(self) -> None:
        result = self.run_helper(self.daemon, self.cli, path=str(self.root / "empty"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("ldd is required", result.stderr)

    def test_missing_or_symlink_artifact_is_rejected(self) -> None:
        missing = self.root / "missing"
        result = self.run_helper(missing)
        self.assertEqual(result.returncode, 1)
        self.assertIn("regular non-symlink", result.stderr)

        alias = self.root / "alias"
        alias.symlink_to(self.daemon)
        result = self.run_helper(alias)
        self.assertEqual(result.returncode, 1)
        self.assertIn("regular non-symlink", result.stderr)

    def test_no_artifact_is_usage_error(self) -> None:
        result = self.run_helper()
        self.assertEqual(result.returncode, 2)
        self.assertIn("Usage:", result.stderr)


if __name__ == "__main__":
    unittest.main()
