#!/usr/bin/env python3




















from __future__ import annotations

import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MAKEFILE_TEXT = (ROOT / "Makefile").read_text(encoding="utf-8")


def makefile_words(name: str) -> set[str]:

    prefix = f"{name} = "
    line = next(
        (candidate for candidate in MAKEFILE_TEXT.splitlines()
         if candidate.startswith(prefix)),
        None,
    )
    if line is None:
        raise AssertionError(f"Makefile variable missing: {name}")
    return set(line.removeprefix(prefix).split())


class DevMakePerformanceContractTest(unittest.TestCase):


    def run_make(self, *args: str) -> str:
        result = subprocess.run(
            ["make", "--no-print-directory", "-B", "-n", *args],
            cwd=ROOT,
            text=True,
            capture_output=True,
            check=True,
        )
        return result.stdout + result.stderr

    def test_single_forwards_selected_parallelism_and_fast_mode(self) -> None:
        output = self.run_make("single", "DEV_JOBS=7", "DEV_USE_MOLD=0")
        self.assertIn("-j7", output)
        self.assertIn("DEV_FAST=1", output)
        self.assertIn("DEV_USE_MOLD=0", output)

    def test_single_respects_an_explicit_parent_job_limit(self) -> None:
        result = subprocess.run(
            [
                "make",
                "--no-print-directory",
                "-B",
                "-n",
                "-j1",
                "single",
                "DEV_JOBS=7",
                "DEV_USE_MOLD=0",
            ],
            cwd=ROOT,
            text=True,
            capture_output=True,
            check=True,
        )
        output = result.stdout + result.stderr
        self.assertNotIn("-j7 DEV_FAST=1", output)

    def test_debug_fast_mode_uses_mold_when_available(self) -> None:
        output = self.run_make(
            "single",
            "DEV_JOBS=7",
            "DEV_USE_MOLD=1",
            "MOLD_BIN=/usr/bin/mold",
        )
        self.assertIn("-fuse-ld=mold", output)

    def test_release_never_uses_mold(self) -> None:
        output = self.run_make(
            "single",
            "BUILD=release",
            "DEV_JOBS=7",
            "DEV_USE_MOLD=1",
            "MOLD_BIN=/usr/bin/mold",
        )
        self.assertNotIn("-fuse-ld=mold", output)

    def test_dev_verify_keeps_test_before_check(self) -> None:
        output = self.run_make("dev-verify", "DEV_JOBS=7", "DEV_USE_MOLD=0")
        self.assertLess(output.index("test-auto"), output.index("dev-check"))

    def test_dev_check_partitions_all_40_public_gates_without_overlap(self) -> None:
        check_all_line = next(
            line for line in MAKEFILE_TEXT.splitlines()
            if line.startswith("check-all:")
        )
        all_gates = set(check_all_line.split(":", 1)[1].split())
        parallel = makefile_words("DEV_PARALLEL_CHECKS")
        mutating = makefile_words("DEV_MUTATING_CHECKS")

        self.assertEqual(parallel & mutating, set())
        self.assertEqual(parallel | mutating, all_gates)
        self.assertEqual(len(all_gates), 40)
        self.assertEqual(
            mutating,
            {
                "check-rpc-consumers",
                "check-rpc-param-contract",
                "check-safety-controls",
                "check-fe-rpc-params",
                "check-rerror-guard",
            },
        )


if __name__ == "__main__":
    unittest.main()
