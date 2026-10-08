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


class JobPersistenceContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="pcv-job-contract-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.base = Path(cls.directory.name)
        compiler = shutil.which("gcc-14") or shutil.which("cc")
        if not compiler:
            raise RuntimeError("C23 compiler required")
        flags = shlex.split(subprocess.check_output([
            "pkg-config", "--cflags", "--libs", "gio-2.0", "json-glib-1.0",
            "libsoup-3.0", "libvirt", "sqlite3"], text=True))
        queue_source = os.environ.get("PCV_JOB_TEST_QUEUE_SOURCE", "src/utils/pcv_job_queue.c")
        common = [compiler, "-std=gnu23", "-D_GNU_SOURCE", "-DPCV_CLUSTER_ENABLED=0",
                  "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-g",
                  "-ffunction-sections", "-fdata-sections", "-I.", "-Isrc", "-Iinclude",
                  "-Iinclude/purecvisor", "-Wl,--gc-sections", queue_source]
        extra = shlex.split(os.environ.get("PCV_JOB_TEST_CFLAGS", ""))
        cls.unit_binary = cls.base / "unit"
        cls.worker_binary = cls.base / "worker"
        commands = [common + ["tests/fixtures/job_queue_runner.c", "tests/test_job_queue.c",
                              "-o", str(cls.unit_binary), *flags, *extra],
                    common + [os.environ.get("PCV_JOB_TEST_WORKER_FIXTURE", "tests/fixtures/job_persistence_contract.c"), "src/api/drain.c",
                              "src/modules/dispatcher/rpc_utils.c", "src/modules/dispatcher/rpc_completion.c",
                              "src/utils/pcv_validate.c",
                              "-Wl,--wrap=soup_websocket_connection_send_text",
                              "-o", str(cls.worker_binary), *flags, *extra]]
        for command in commands:
            result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=90)
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)

    def run_worker(self, mode):
        database = self.base / f"{mode}.db"
        result = subprocess.run([str(self.worker_binary), mode, str(database)],
                                cwd=ROOT, capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        frame = json.loads(next(line[6:] for line in result.stdout.splitlines() if line.startswith("FRAME ")))
        self.assertEqual(frame["type"], "job.complete")
        with sqlite3.connect(f"file:{database}?mode=ro", uri=True) as connection:
            rows = connection.execute("SELECT job_id,target,status,result FROM jobs").fetchall()
        print(result.stdout.strip(), flush=True)
        return frame["payload"], rows, result.stdout

    def test_native_database_regressions(self):
        result = subprocess.run([str(self.unit_binary)], cwd=ROOT,
                                capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(len(re.findall(r"^ok \d+ /job_queue/", result.stdout, re.M)), 21)

    def test_all_admission_callers_guard_before_accepted(self):


        paths = ["src/api/dispatcher.c", "src/modules/dispatcher/handler_backup.c",
                 "src/modules/dispatcher/handler_vpc.c", "src/modules/dispatcher/handler_accel.c"]
        total = 0
        for path in paths:
            source = (ROOT / path).read_text()
            source = re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"',
                            lambda match: " " if match[0].startswith(("/*", "//")) else match[0],
                            source, flags=re.S)
            for call in re.finditer(r'(?P<var>(?:data->)?job_id)\s*=\s*pcv_job_create\([^;]+;', source):
                total += 1
                tail = source[call.end():]
                guard = re.match(r'\s*(?:g_free\(target\);\s*)?(?:/\*.*?\*/\s*)?if\s*\(!' + re.escape(call["var"]) + r'\)\s*\{(?P<body>[^}]+)\}', tail, re.S)
                self.assertIsNotNone(guard, f"{path}: missing admission guard")
                self.assertIn("PURE_RPC_ERR_INTERNAL_ERROR", guard["body"])
                self.assertIn("return;", guard["body"])
                self.assertNotIn("g_task_run_in_thread", guard["body"])
                if '"vm.create"' in call[0]:
                    self.assertIn("unlock_vm_operation(name)", guard["body"])
                if '"ova_export"' in call[0]:
                    self.assertIn("free(real_out)", guard["body"])
                if '"ova_import"' in call[0]:
                    self.assertIn("free(real_ova)", guard["body"])
        self.assertEqual(total, 8)

    def test_worker_success_persists_and_survives_exit(self):
        payload, rows, text = self.run_worker("success")
        self.assertEqual(payload["status"], "completed")
        self.assertIs(payload["result_persisted"], True)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0][:3], (payload["job_id"], "probe@snapshot", 2))
        self.assertEqual(json.loads(rows[0][3]), {"vm_name": "probe", "snapshot_name": "snapshot"})
        self.assertIn("EFFECTS 1 AUDIT 1 ok", text)

    def test_worker_success_with_locked_result_keeps_actual_outcome(self):
        payload, rows, text = self.run_worker("locked-success")
        self.assertEqual(payload["status"], "completed")
        self.assertIs(payload["result_persisted"], False)
        self.assertEqual(rows, [(payload["job_id"], "probe@snapshot", 1, None)])
        self.assertIn("EFFECTS 1 AUDIT 1 ok", text)

    def test_worker_failure_with_locked_result_keeps_error_and_audit(self):
        payload, rows, text = self.run_worker("locked-failure")
        self.assertEqual(payload["status"], "failed")
        self.assertIs(payload["result_persisted"], False)
        self.assertEqual(payload["error"], 'backend failed\n"quoted" \\path')
        self.assertEqual(rows, [(payload["job_id"], "probe@snapshot", 1, None)])
        self.assertIn("AUDIT 1 fail", text)

    def test_denied_admission_has_no_worker_then_recovers(self):
        payload, rows, text = self.run_worker("admission")
        denied = json.loads(next(line[7:] for line in text.splitlines() if line.startswith("DENIED ")))
        accepted = json.loads(next(line[9:] for line in text.splitlines() if line.startswith("ACCEPTED ")))
        self.assertEqual(denied["error"]["code"], -32603)
        self.assertNotIn("result", denied)
        self.assertEqual(accepted["result"]["job_id"], payload["job_id"])
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0][2], 2)
        self.assertIn("EFFECTS 1 AUDIT 1 ok", text)

    def test_legacy_event_has_no_persistence_claim(self):
        payload, rows, text = self.run_worker("legacy")
        self.assertNotIn("result_persisted", payload)
        self.assertEqual(payload["status"], "completed")
        self.assertEqual(rows, [])
        self.assertIn("EFFECTS 0 AUDIT 0 none", text)


if __name__ == "__main__":
    unittest.main(verbosity=2)
