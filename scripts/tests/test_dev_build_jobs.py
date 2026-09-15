#!/usr/bin/env python3




















from __future__ import annotations

import os
import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "dev_build_jobs.sh"


class DevBuildJobsTest(unittest.TestCase):


    def run_detector(
        self,
        *,
        logical: int = 72,
        physical: int = 36,
        memory_mib: int = 112_640,
        override: str | None = None,
        check: bool = True,
    ) -> subprocess.CompletedProcess[str]:
        env = os.environ.copy()
        env.update(
            {
                "PCV_BUILD_TEST_LOGICAL": str(logical),
                "PCV_BUILD_TEST_PHYSICAL": str(physical),
                "PCV_BUILD_TEST_MEM_AVAILABLE_MIB": str(memory_mib),
            }
        )
        if override is None:
            env.pop("PCV_BUILD_JOBS", None)
        else:
            env["PCV_BUILD_JOBS"] = override
        return subprocess.run(
            [str(SCRIPT)],
            cwd=ROOT,
            env=env,
            text=True,
            capture_output=True,
            check=check,
        )

    def test_dual_socket_72_thread_host_uses_measured_60_jobs(self) -> None:
        self.assertEqual(self.run_detector().stdout.strip(), "60")

    def test_old_8_core_16_thread_host_uses_13_jobs(self) -> None:
        result = self.run_detector(logical=16, physical=8, memory_mib=32_768)
        self.assertEqual(result.stdout.strip(), "13")

    def test_non_smt_host_uses_all_logical_cpus(self) -> None:
        result = self.run_detector(logical=8, physical=8, memory_mib=32_768)
        self.assertEqual(result.stdout.strip(), "8")

    def test_available_memory_caps_compiler_count(self) -> None:
        result = self.run_detector(memory_mib=4_096)
        self.assertEqual(result.stdout.strip(), "8")

    def test_positive_operator_override_wins(self) -> None:
        result = self.run_detector(override="7")
        self.assertEqual(result.stdout.strip(), "7")

    def test_invalid_operator_override_fails_closed(self) -> None:
        for override in ("0", "-1", "abc", "1.5"):
            with self.subTest(override=override):
                result = self.run_detector(override=override, check=False)
                self.assertEqual(result.returncode, 2)
                self.assertIn("PCV_BUILD_JOBS", result.stderr)


if __name__ == "__main__":
    unittest.main()
