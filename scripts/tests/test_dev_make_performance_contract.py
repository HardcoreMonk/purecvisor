#!/usr/bin/env python3




















from __future__ import annotations

import os
import re
import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MAKEFILE_TEXT = (ROOT / "Makefile").read_text(encoding="utf-8")




_INHERITED_MAKE_VARS = ("MAKEFLAGS", "MFLAGS", "MAKELEVEL", "MAKEOVERRIDES", "GNUMAKEFLAGS", "BUILD")


def clean_make_env() -> dict[str, str]:

    env = dict(os.environ)
    for name in _INHERITED_MAKE_VARS:
        env.pop(name, None)
    return env


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
            env=clean_make_env(),
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
            env=clean_make_env(),
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

    def check_all_gates(self) -> list[str]:

        check_all_line = next(
            line for line in MAKEFILE_TEXT.splitlines()
            if line.startswith("check-all:")
        )
        return check_all_line.split(":", 1)[1].split()

    def test_dev_check_partitions_all_check_all_gates_without_overlap(self) -> None:

        gates = self.check_all_gates()
        all_gates = set(gates)
        parallel = makefile_words("DEV_PARALLEL_CHECKS")
        mutating = makefile_words("DEV_MUTATING_CHECKS")

        self.assertEqual(len(gates), len(all_gates), "check-all 의존 중복")
        self.assertEqual(parallel & mutating, set())
        self.assertEqual(parallel | mutating, all_gates)
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

    def test_check_all_success_message_matches_gate_list(self) -> None:

        count = len(self.check_all_gates())
        block = re.search(
            r"(?m)^check-all:[^\n]*\n(?P<recipe>(?:\t[^\n]*\n)+)",
            MAKEFILE_TEXT,
        )
        self.assertIsNotNone(block)
        message = re.search(r"전체 통과 \((\d+)게이트: (.*)\)\"", block["recipe"])
        self.assertIsNotNone(message)
        self.assertEqual(int(message.group(1)), count)
        labels = [label.strip() for label in message.group(2).split(" + ")]
        self.assertEqual(len(labels), count, labels)
        self.assertEqual(len(set(labels)), count, "완료 문구 항목 중복")
        self.assertIn("VNC proxy lifetime", labels)

    def make_database(self, goal: str) -> str:

        result = subprocess.run(
            ["make", "--no-print-directory", "-p", "-n", "-q", goal],
            cwd=ROOT,
            text=True,
            capture_output=True,
            check=False,
            env=clean_make_env(),
        )
        return result.stdout

    @staticmethod
    def order_only(database: str, target: str) -> list[str]:
        match = re.search(rf"(?m)^{re.escape(target)}:([^\n]*)$", database)
        if match is None or "|" not in match.group(1):
            return []
        return match.group(1).split("|", 1)[1].split()

    def test_check_all_serializes_mutating_gates_after_parallel_gates(self) -> None:

        database = self.make_database("check-all")
        parallel = makefile_words("DEV_PARALLEL_CHECKS")
        line = next(line for line in MAKEFILE_TEXT.splitlines()
                    if line.startswith("DEV_MUTATING_CHECKS = "))
        ordered = line.removeprefix("DEV_MUTATING_CHECKS = ").split()
        self.assertEqual(set(self.order_only(database, ordered[0])), parallel)
        for previous, gate in zip(ordered, ordered[1:]):
            self.assertEqual(self.order_only(database, gate), [previous], gate)

    def test_single_gate_runs_without_check_all_ordering(self) -> None:

        database = self.make_database("check-rerror-guard")
        self.assertEqual(self.order_only(database, "check-rerror-guard"), [])

if __name__ == "__main__":
    unittest.main()
