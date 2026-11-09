#!/usr/bin/env python3
                                                                  
                                                       
import importlib.util
import hashlib
import json
import os
import sqlite3
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class JobRecovery(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="pcv-job-recovery-")
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.binary = Path(cls.tmp.name) / "probe"
        flags = shlex.split(subprocess.check_output([
            "pkg-config", "--cflags", "--libs", "gio-2.0", "json-glib-1.0", "sqlite3"], text=True))
        command = [shutil.which("gcc-14") or "cc", "-std=gnu23", "-D_GNU_SOURCE",
                   "-Wall", "-Wextra", "-Werror", "-g", "-Isrc", "src/utils/pcv_job_queue.c",
                   "src/utils/pcv_job_recovery.c", "src/api/drain.c", "src/modules/dispatcher/rpc_completion.c", "tests/fixtures/job_recovery_probe.c", "-o", str(cls.binary), *flags,
                   *["-Wl,--wrap=" + fn for fn in ["write", "fsync", "renameat", "unlinkat",
                      "sqlite3_exec", "sqlite3_bind_text", "g_get_monotonic_time", "g_timeout_add_seconds"]],
                   *shlex.split(os.environ.get("PCV_JOB_TEST_CFLAGS", ""))]
        result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
        if result.returncode: raise RuntimeError(result.stdout + result.stderr)

    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix="pcv-restart-")
        self.addCleanup(tmp.cleanup)
        self.database = Path(tmp.name) / "jobs.db"
        self.journal = Path(str(self.database) + ".journal")
        self.effects = Path(str(self.database) + ".effects")

    def run_probe(self, mode, expected=0):
        result = subprocess.run([*shlex.split(os.environ.get("PCV_JOB_TEST_RUNNER", "")), self.binary, mode, self.database], capture_output=True,
                                text=True, timeout=45)
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        return result.stdout

    def read(self, mode="read"):
        text = self.run_probe(mode)
        return json.loads(next(line for line in text.splitlines() if line.startswith("[")))

    def event(self, mode):
        text = self.run_probe(mode)
        return json.loads(next(line[6:] for line in text.splitlines() if line.startswith("EVENT "))), text

    def effect_count(self):
        return len(self.effects.read_text().splitlines()) if self.effects.exists() else 0

    def record(self):
        paths = list(self.journal.glob("*.json"))
        self.assertEqual(len(paths), 1)
        return paths[0], paths[0].read_bytes()

    def replace_payload(self, path, changes):
        outer = json.loads(path.read_bytes())
        obj = json.loads(outer["payload"])
        obj.update(changes)
        payload = json.dumps(obj, separators=(",", ":"))
        path.write_text(json.dumps({"payload": payload, "sha256": hashlib.sha256(payload.encode()).hexdigest()}))
        path.chmod(0o600)
        return obj

    def test_nullable_binding_failure_rejects_admission(self):
        self.assertIn("REJECTED", self.run_probe("bind-admit"))
        self.assertEqual(self.read(), [])
        self.assertEqual(self.effect_count(), 0)

    def test_nullable_result_binding_failure_never_claims_persisted(self):
        event, _ = self.event("bind-result")
        self.assertIs(event["result_persisted"], False)
        self.assertIs(event["recovery_pending"], True)
        self.record()
        self.assertEqual(json.loads(self.read()[0]["result"]), {"effect": 1})
        self.assertEqual(self.effect_count(), 1)

    def test_startup_db_lock_defers_reconcile_and_admission_until_recovery(self):
        self.run_probe("tracked")
        with sqlite3.connect(self.database) as connection:
            connection.execute("BEGIN IMMEDIATE")
            process = subprocess.Popen([*shlex.split(os.environ.get("PCV_JOB_TEST_RUNNER", "")),
                                        self.binary, "startup-locked", self.database],
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            self.addCleanup(lambda: process.kill() if process.poll() is None else None)
            self.assertEqual(process.stdout.readline().strip(), "STARTUP_WAIT")
            connection.rollback()
            text, error = process.communicate(timeout=10)
            self.assertEqual(process.returncode, 0, text + error)
            self.assertIn("ADMISSION_RESUMED", text)
            self.assertIn("CURRENT_JOB_PENDING", text)
            rows = json.loads(next(line for line in text.splitlines() if line.startswith("[")))
            self.assertEqual(rows[0]["status"], "interrupted")
            self.assertIs(rows[0]["outcome_known"], False)
        self.assertEqual(self.effect_count(), 0)

    def test_startup_reconcile_is_atomic_and_retry_exhaustion_keeps_admission_closed(self):
        self.run_probe("capacity")
        text = self.run_probe("startup-reconcile-bind")
        self.assertIn("RECONCILE_RETRIES 32", text)
        rows = json.loads(next(line for line in text.splitlines() if line.startswith("[")))
        self.assertEqual(len(rows), 128)
        self.assertTrue(all(row["status"] == "pending" for row in rows))
        times = [int(line.split()[2]) for line in text.splitlines() if line.startswith("REPLAY ")]
        self.assertEqual(len(times), 32)
        for index, (before, after) in enumerate(zip(times, times[1:])):
            self.assertGreaterEqual(after - before, min(60, 2 ** index) * 1_000_000)
        self.assertTrue(all(row["status"] == "interrupted" for row in self.read()))
        self.assertEqual(self.effect_count(), 0)

    def test_prior_pending_is_interrupted_without_execution(self):
        first = self.run_probe("admit").strip()
        row = self.read()[0]
        self.assertEqual(row["job_id"], first)
        self.assertEqual(row["status"], "interrupted")
        self.assertIs(row["outcome_known"], False)
        self.assertEqual(self.effect_count(), 0)

    def test_tracked_generation_is_captured_at_admission(self):
        text = self.run_probe("tracked")
        row = json.loads(next(line for line in text.splitlines() if line.startswith("[")))[0]
        self.assertEqual(row["status"], "pending")
        self.assertEqual(row["method"], "probe.finish")
        self.assertEqual(len(row["generation"]), 36)
        self.assertIn(row["generation"], text)
        self.assertEqual(self.read()[0]["status"], "interrupted")

    def test_durable_result_replays_after_db_and_notification_loss(self):
        event, _ = self.event("locked")
        self.assertIs(event["result_persisted"], False)
        self.assertIs(event["recovery_pending"], True)
        path, data = self.record()
        self.assertEqual(path.stat().st_mode & 0o777, 0o600)
        self.assertNotIn(b"request-canary", data)
        outer = json.loads(data)
        self.assertEqual(hashlib.sha256(outer["payload"].encode()).hexdigest(), outer["sha256"])
        with sqlite3.connect(self.database) as db:
            self.assertEqual(db.execute("SELECT status,result FROM jobs").fetchone(), (1, None))
        row = self.read()[0]
        self.assertEqual(row["status"], "completed")
        self.assertIs(row["outcome_known"], True)
        self.assertIs(row["recovery_pending"], False)
        self.assertEqual(json.loads(row["result"]), {"effect": 1})
        self.assertEqual(list(self.journal.iterdir()), [])
        self.assertEqual(self.effect_count(), 1)
        self.assertEqual(self.read(), [row])

    def test_success_and_failure_are_separate_from_storage(self):
        event, _ = self.event("failure")
        self.assertEqual(event["status_code"], 3)
        self.assertEqual(event["error"], 'backend failed\n"quoted" \\path')
        self.assertIs(event["result_persisted"], True)
        self.assertEqual(self.read()[0]["status"], "failed")
        self.assertEqual(self.effect_count(), 1)

    def test_crash_windows_do_not_repeat_effect(self):
        for mode, code, terminal in [
            ("crash-partial", 80, "interrupted"), ("crash-effect", 81, "interrupted"),
            ("crash-rename", 83, "completed"), ("crash-db-before", 84, "completed"),
            ("crash-db-after", 85, "completed"), ("crash-delete", 86, "completed")]:
            with self.subTest(mode=mode):
                                                             
                original = (self.database, self.journal, self.effects)
                self.database = original[0].with_name(mode + ".db")
                self.journal = Path(str(self.database) + ".journal")
                self.effects = Path(str(self.database) + ".effects")
                self.run_probe(mode, code)
                self.assertEqual(self.read()[0]["status"], terminal)
                self.assertEqual(self.effect_count(), 1)
                self.read()
                self.assertEqual(self.effect_count(), 1)
                if mode == "crash-partial": self.assertTrue(list(self.journal.glob("*.quarantine")))
                self.database, self.journal, self.effects = original

    def test_admission_commit_crash_has_no_effect(self):
        self.run_probe("crash-admit-before", 84)
        self.assertEqual(self.read(), [])
        self.run_probe("crash-admit-after", 85)
        self.assertEqual(self.read()[0]["status"], "interrupted")
        self.assertEqual(self.effect_count(), 0)

    def test_corruption_is_preserved_and_cannot_invent_success(self):
        self.event("locked")
        path, data = self.record()
        path.write_bytes(data.replace(b"sha256", b"broken", 1))
        row = self.read()[0]
        self.assertEqual(row["status"], "interrupted")
        self.assertIs(row["outcome_known"], False)
        quarantine = list(self.journal.glob("*.quarantine"))
        self.assertEqual(len(quarantine), 1)
        self.assertEqual(quarantine[0].read_bytes(), path.read_bytes() if path.exists() else data.replace(b"sha256", b"broken", 1))
        self.read("cleanup")
        self.assertTrue(quarantine[0].exists())
        self.assertEqual(self.effect_count(), 1)

    def test_old_generation_cannot_overwrite_new_row(self):
        self.event("locked")
        path, data = self.record()
        with sqlite3.connect(self.database) as db:
            db.execute("UPDATE jobs SET generation='11111111-1111-4111-8111-111111111111',status=2,result='new-result',outcome_known=1")
        row = self.read()[0]
        self.assertEqual(row["result"], "new-result")
        self.assertTrue(list(self.journal.glob("*.quarantine")))
        self.assertEqual(self.read("cleanup")[0]["result"], "new-result")
        self.assertEqual(self.effect_count(), 1)

    def test_other_target_and_method_cannot_overwrite(self):
        self.event("locked")
        path, _ = self.record()
        self.replace_payload(path, {"target": "another-target", "method": "another.method"})
        row = self.read()[0]
        self.assertEqual(row["target"], "identity")
        self.assertEqual(row["status"], "interrupted")
        self.assertTrue(list(self.journal.glob("*.quarantine")))

    def test_repeated_identical_replay_is_idempotent(self):
        self.event("locked")
        path, data = self.record()
        row = self.read()[0]
        for _ in range(3):
            path.write_bytes(data); path.chmod(0o600)
            self.assertEqual(self.read()[0]["result"], row["result"])
            self.assertFalse(path.exists())
        self.assertEqual(self.effect_count(), 1)

    def test_cancel_and_late_progress_do_not_overwrite_terminal(self):
        event, _ = self.event("cancel")
        self.assertIs(event["result_persisted"], False)
        self.assertIs(event["recovery_pending"], False)
        self.assertEqual(event["reason"], "identity_or_terminal_conflict")
        row = self.read()[0]
        self.assertEqual(row["status"], "cancelled")
        self.assertIs(row["outcome_known"], False)
        self.assertEqual(self.effect_count(), 1)

    def test_short_write_is_completed_and_fsynced(self):
        event, _ = self.event("short-write")
        self.assertIs(event["result_persisted"], True)
        self.assertEqual(self.read()[0]["status"], "completed")

    def test_storage_failures_are_explicit_and_preserved(self):
        for mode in ["enospc", "fsync-fail", "oversize"]:
            with self.subTest(mode=mode):
                original = (self.database, self.journal, self.effects)
                self.database = original[0].with_name(mode + ".db")
                self.journal = Path(str(self.database) + ".journal")
                self.effects = Path(str(self.database) + ".effects")
                event, _ = self.event(mode)
                self.assertIs(event["result_persisted"], False)
                self.assertIs(event["recovery_pending"], False)
                self.assertEqual(self.read()[0]["status"], "interrupted")
                self.assertEqual(self.effect_count(), 1)
                self.database, self.journal, self.effects = original

    def test_one_writer_keeps_main_context_running(self):
        event, text = self.event("slow-writer")
        self.assertIs(event["result_persisted"], True)
        count = int(next(line.split()[1] for line in text.splitlines() if line.startswith("HEARTBEATS")))
        self.assertGreater(count, 100)

    def test_burst_uses_single_writer_and_completes_each_job_once(self):
        text = self.run_probe("burst")
        self.assertIn("IO_PEAK 1 DELIVERIES 128", text)
        events = [json.loads(line[6:]) for line in text.splitlines() if line.startswith("EVENT ")]
        self.assertEqual(len({event["job_id"] for event in events}), 128)
        self.assertTrue(all(event["result_persisted"] and not event["recovery_pending"] for event in events))
        self.assertEqual(len(self.read()), 128)
        self.assertEqual(self.effect_count(), 128)
        self.assertEqual(list(self.journal.iterdir()), [])

    def test_admission_reserves_bounded_capacity(self):
        self.assertIn("CAPACITY 128", self.run_probe("capacity"))
        with sqlite3.connect(self.database) as db:
            self.assertEqual(db.execute("SELECT count(*) FROM jobs").fetchone()[0], 128)
        self.assertEqual(self.effect_count(), 0)

    def test_quarantine_and_metadata_count_toward_disk_limit(self):
        self.journal.mkdir(mode=0o700)
        for i in range(256): (self.journal / f"{i}.quarantine").write_bytes(b"x")
        self.assertIn("REJECTED", self.run_probe("reject"))
        self.assertEqual(len(list(self.journal.iterdir())), 256)
        self.assertEqual(self.effect_count(), 0)

    def test_retry_is_bounded_and_restart_begins_new_round(self):
        event, text = self.event("retry-exhaust")
        self.assertIs(event["recovery_pending"], True)
        self.assertIn("RETRIES 32", text)
        times = [int(line.split()[2]) for line in text.splitlines() if line.startswith("REPLAY ")]
        self.assertEqual(len(times), 32)
        for index, (before, after) in enumerate(zip(times, times[1:])):
            self.assertGreaterEqual(after - before, min(60, 2 ** index) * 1_000_000)
        row = json.loads(next(line for line in text.splitlines() if line.startswith("[")))[0]
        self.assertEqual(row["status"], "running")
        self.assertEqual(row["recovery_reason"], "retry_exhausted")
        self.assertEqual(self.read()[0]["status"], "completed")
        self.assertEqual(self.effect_count(), 1)

    def test_terminal_conflict_preserves_record(self):
        self.event("locked")
        with sqlite3.connect(self.database) as db:
            db.execute("UPDATE jobs SET status=3,result='different',outcome_known=1")
        self.assertEqual(self.read()[0]["result"], "different")
        self.assertTrue(list(self.journal.glob("*.quarantine")))

    def test_byte_capacity_includes_unresolved_metadata(self):
        self.journal.mkdir(mode=0o700)
        path = self.journal / "metadata"
        with path.open("wb") as stream: stream.truncate(16 * 1024 * 1024 + 1)
        self.assertIn("REJECTED", self.run_probe("reject"))
        self.assertEqual(self.effect_count(), 0)
        self.assertEqual(sum(p.stat().st_size for p in self.journal.iterdir()), 16 * 1024 * 1024 + 1)

    def test_completion_gate_rejects_removed_writer_and_worker(self):
        spec = importlib.util.spec_from_file_location("job_audit_gate", ROOT / "scripts/check_audit_placement.py")
        gate = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(gate)
        with tempfile.TemporaryDirectory(prefix="pcv-job-gate-") as tmp:
            gate.REPO_ROOT = Path(tmp)
            for path in ["src/api/ws_server.c", "src/utils/pcv_job_recovery.c",
                         "src/api/dispatcher.c", "src/modules/dispatcher/handler_vm_ova.c", "src/modules/dispatcher/handler_vm_create.c", "src/modules/dispatcher/handler_backup.c",
                         "src/modules/dispatcher/handler_accel.c", "src/modules/dispatcher/handler_vpc.c",
                         *(p.relative_to(ROOT).as_posix() for p in gate.rpc_extract.vm_family("vm_lifecycle"))]:
                target = Path(tmp) / path
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes((ROOT / path).read_bytes())
            self.assertIn("vm.import.ova", gate.collect_durable_completion_methods())
            ws = Path(tmp) / "src/api/ws_server.c"
            original = ws.read_text()
            ws.write_text(original.replace("pcv_job_finish_async(ref, status, result, error, _job_result_ready", "lost_writer(ref, status, result, error, _job_result_ready"))
            self.assertEqual(gate.collect_durable_completion_methods(), set())
            ws.write_text(original)
            dispatcher = Path(tmp) / "src/modules/dispatcher/handler_vm_ova.c"
            text = dispatcher.read_text()
            start = text.index("static void _ova_import_worker")
            end = text.index("\nvoid handle_vm_import_ova(", start)
            dispatcher.write_text(text[:start] + text[start:end].replace("pcv_ws_finish_job(ctx->job_ref", "lost_completion(ctx->job_ref") + text[end:])
            self.assertNotIn("vm.import.ova", gate.collect_durable_completion_methods())
            self.assertIn("vm.export.ova", gate.collect_durable_completion_methods())

    def legacy_db(self, extra=""):
        with sqlite3.connect(self.database) as db:
            db.execute("CREATE TABLE jobs(job_id TEXT PRIMARY KEY,type TEXT NOT NULL,target TEXT,status INTEGER DEFAULT 0,progress INTEGER DEFAULT 0,detail TEXT,params TEXT,result TEXT,created_at INTEGER,updated_at INTEGER" + extra + ")")
            db.execute("INSERT INTO jobs(job_id,type,target,status,result) VALUES('job-12345678','ova_export','legacy-vm',2,'legacy-result')")

    def test_legacy_schema_upgrade_keeps_result_and_assigns_identity(self):
        self.legacy_db()
        row = self.read()[0]
        self.assertEqual(row["result"], "legacy-result")
        self.assertEqual(row["method"], "vm.export.ova")
        self.assertEqual(len(row["generation"]), 36)
        with sqlite3.connect(self.database) as db: self.assertEqual(db.execute("PRAGMA user_version").fetchone()[0], 2)

    def test_failed_migration_rolls_back_and_rejects_admission(self):
        self.legacy_db(",method TEXT")
        with sqlite3.connect(self.database) as db:
            before = db.execute("SELECT type,name,sql FROM sqlite_master ORDER BY type,name").fetchall()
        self.assertIn("REJECTED", self.run_probe("reject"))
        with sqlite3.connect(self.database) as db:
            self.assertEqual(db.execute("PRAGMA user_version").fetchone()[0], 0)
            self.assertNotIn("generation", [row[1] for row in db.execute("PRAGMA table_info(jobs)")])
            self.assertEqual(db.execute("SELECT type,name,sql FROM sqlite_master ORDER BY type,name").fetchall(), before)
            self.assertEqual(db.execute("SELECT result FROM jobs").fetchone()[0], "legacy-result")
        self.assertEqual(self.effect_count(), 0)

    def test_future_schema_is_rejected_without_loss(self):
        self.legacy_db()
        with sqlite3.connect(self.database) as db: db.execute("PRAGMA user_version=99")
        self.assertIn("REJECTED", self.run_probe("reject"))
        with sqlite3.connect(self.database) as db: self.assertEqual(db.execute("PRAGMA user_version").fetchone()[0], 99)


if __name__ == "__main__":
    unittest.main(verbosity=2)
