#!/usr/bin/env python3




import json
import os
from pathlib import Path
import re
import shlex
import shutil
import sqlite3
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class AsyncShutdownLifetime(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="pcv-async-shutdown-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.base = Path(cls.directory.name)
        cls.binary = cls.base / "probe"
        compiler = shutil.which("gcc-14") or shutil.which("gcc")
        if not compiler:
            raise RuntimeError("C23 compiler required")
        packages = ["gio-2.0", "json-glib-1.0", "libvirt", "libsoup-3.0"]
        flags = shlex.split(subprocess.check_output(
            ["pkg-config", "--cflags", "--libs", *packages], text=True))
        command = [compiler, "-std=gnu23", "-D_GNU_SOURCE", "-DPCV_CLUSTER_ENABLED=0",
                   "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-g",
                   "-ffunction-sections", "-fdata-sections", "-I.", "-Isrc",
                   "-Iinclude", "-Iinclude/purecvisor",
                   "tests/fixtures/async_shutdown_lifetime.c", "src/api/drain.c",
                   "src/utils/pcv_job_queue.c", "src/utils/pcv_worker_pool.c",
                   "-Wl,--gc-sections", "-Wl,--wrap=g_thread_pool_push", "-lsqlite3", "-o", str(cls.binary), *flags,
                   *shlex.split(os.environ.get("PCV_ASYNC_TEST_CFLAGS", ""))]
        result = subprocess.run(command, cwd=ROOT, text=True, capture_output=True, timeout=90)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)

    def test_completed_result_survives_process_exit(self):
        for mode in ("shared", "pool", "pool-push-error", "fallback", "no-return", "callback", "destructor",
                     "cancel", "chain", "held-ref", "context", "manual", "failure"):
            with self.subTest(mode=mode):
                database = self.base / f"{mode}.db"
                result = subprocess.run([str(self.binary), mode, str(database)],
                                        text=True, capture_output=True, timeout=15)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                with sqlite3.connect(f"file:{database}?mode=ro", uri=True) as connection:
                    rows = connection.execute("SELECT status,result FROM jobs").fetchall()
                self.assertEqual(len(rows), 1)
                self.assertEqual(rows[0][0], 3 if mode == "failure" else 2, result.stdout)
                expected = {"error": "fixture backend failure"} if mode == "failure" else {
                    "vm_name": "audit-probe", "snapshot_name": "snapshot"}
                self.assertEqual(json.loads(rows[0][1]), expected)
                print(result.stdout.strip(), flush=True)

    def test_timeout_preserves_incomplete_status_and_skips_cleanup(self):
        database = self.base / "timeout.db"
        result = subprocess.run([str(self.binary), "timeout", str(database)],
                                text=True, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("exiting unsuccessfully without unsafe cleanup", result.stderr)
        self.assertNotIn("Resources released", result.stderr)
        with sqlite3.connect(f"file:{database}?mode=ro", uri=True) as connection:
            self.assertEqual(connection.execute("SELECT status,result FROM jobs").fetchall(),
                             [(1, None)])

    def test_production_task_creation_cannot_bypass_lifetime_registration(self):


        ignored = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', re.S)
        callsites = []
        for path in (ROOT / "src").rglob("*.c"):
            code = ignored.sub(" ", path.read_text())
            if re.search(r"\bg_task_new\s*\(", code):
                callsites.append(str(path.relative_to(ROOT)))
        self.assertEqual(callsites, ["src/api/drain.c"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
