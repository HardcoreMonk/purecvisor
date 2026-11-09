#!/usr/bin/env python3
                                                                   
                                                              
                                                           
                                               
                                                
                                                                                  
import json
import os
from pathlib import Path
import shlex
import shutil
import sqlite3
import subprocess
import sys
import unittest

import test_job_recovery as recovery_fixture

ROOT = recovery_fixture.ROOT

PREVIOUS = "8f7be79a6b7600156018761fc1a6de311d8f3966"


def previous_revision_available():
                                                                         
    try:
        return subprocess.run(["git", "cat-file", "-e", f"{PREVIOUS}^{{commit}}"], cwd=ROOT,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0
    except OSError:
        return False


class JobSchemaRestore(unittest.TestCase):
                                                         
    setUp = recovery_fixture.JobRecovery.setUp
    run_probe = recovery_fixture.JobRecovery.run_probe
    read = recovery_fixture.JobRecovery.read
    effect_count = recovery_fixture.JobRecovery.effect_count

    @classmethod
    def setUpClass(cls):
        if not previous_revision_available():
            reason = f"이전 revision {PREVIOUS[:12]}이 git 이력에 없어 Job 스키마 복원 대조를 건너뛴다"
            print(f"[test_job_schema_restore] {reason}", file=sys.stderr)
            raise unittest.SkipTest(reason)
        recovery_fixture.JobRecovery.setUpClass.__func__(cls)
        previous = Path(cls.tmp.name) / "previous"
        previous.mkdir()
        for name in ("pcv_job_queue.c", "pcv_job_queue.h"):
            data = subprocess.check_output(["git", "show", f"{PREVIOUS}:src/utils/{name}"], cwd=ROOT)
            (previous / name).write_bytes(data)
        cls.previous_binary = previous / "probe"
        flags = shlex.split(subprocess.check_output([
            "pkg-config", "--cflags", "--libs", "json-glib-1.0", "sqlite3"], text=True))
        subprocess.run([shutil.which("gcc-14") or "cc", "-std=gnu23", "-D_GNU_SOURCE",
                        "-Wall", "-Wextra", "-Werror", "-g", "-I" + str(previous), "-Isrc",
                        str(previous / "pcv_job_queue.c"), "tests/fixtures/job_legacy_schema_probe.c",
                        "-o", str(cls.previous_binary), *flags,
                        *shlex.split(os.environ.get("PCV_JOB_TEST_CFLAGS", ""))], cwd=ROOT, check=True)

    def previous(self, mode="read", database=None):
        result = subprocess.run([*shlex.split(os.environ.get("PCV_JOB_TEST_RUNNER", "")),
                                 self.previous_binary, mode, database or self.database],
                                capture_output=True, text=True, timeout=45)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return json.loads(next(line for line in result.stdout.splitlines() if line.startswith("[")))

    def snapshot(self, destination):
        with sqlite3.connect(self.database) as source, sqlite3.connect(destination) as target:
            source.backup(target)
            self.assertEqual(target.execute("PRAGMA integrity_check").fetchone()[0], "ok")

    def test_binary_only_downgrade_cannot_preserve_interrupted_history(self):
        self.run_probe("tracked")
        self.assertEqual(self.read()[0]["status"], "interrupted")
        old = self.previous()[0]
        self.assertEqual(old["status"], "unknown")
        with sqlite3.connect(self.database) as db:
            self.assertEqual(db.execute("SELECT status FROM jobs").fetchone()[0], 5)
            db.execute("UPDATE jobs SET updated_at=1")
                                                                
        self.assertIs(self.read("cleanup")[0]["outcome_known"], False)
        self.assertEqual(self.previous("cleanup"), [])
        with sqlite3.connect(self.database) as db:
            self.assertEqual(db.execute("SELECT count(*) FROM jobs").fetchone()[0], 0)
        print("INCOMPATIBLE: previous status mapping is unknown and previous TTL deletes unconfirmed history")

    def test_previous_writer_on_v2_cannot_supply_generation_and_known_outcome(self):
        self.assertEqual(self.read(), [])
        self.assertEqual(self.previous("create")[0]["status"], "completed")
        with sqlite3.connect(self.database) as db:
            self.assertEqual(db.execute("SELECT generation,method,outcome_known FROM jobs").fetchone(), ("", "", 0))
        current = self.read()[0]
        self.assertEqual(current["generation"], "")
        self.assertIs(current["outcome_known"], False)
        print("INCOMPATIBLE: previous writes on v2 leave empty identity and unconfirmed result")

    def test_wal_snapshot_and_previous_schema_restore_preserve_archived_new_history(self):
        before = self.previous("create")
        backup = self.database.with_name("previous.backup.db")
        raw_copy = self.database.with_name("unsafe-main-only.db")
        with sqlite3.connect(self.database) as keeper:
            keeper.execute("PRAGMA wal_autocheckpoint=0")
            keeper.execute("INSERT INTO jobs(job_id,type,status,result,created_at,updated_at) VALUES(?,?,?,?,?,?)",
                           ("job-00c0ffee", "probe.wal", 2, "wal-result", 1, 1))
            keeper.commit()
            self.assertGreater(Path(str(self.database) + "-wal").stat().st_size, 0)
            shutil.copyfile(self.database, raw_copy)
            self.snapshot(backup)
            self.assertFalse(any(row["job_id"] == "job-00c0ffee" for row in self.previous(database=raw_copy)))
            saved = self.previous(database=backup)
            self.assertTrue(any(row["job_id"] == "job-00c0ffee" for row in saved))
        self.assertEqual(len(saved), len(before) + 1)
        self.run_probe("failure")
        new_rows = self.read()
        post = next(row for row in new_rows if row["type"] == "probe")
        archive = self.database.with_name("candidate-history.db")
        self.snapshot(archive)
        with sqlite3.connect(backup) as source, sqlite3.connect(self.database) as target:
            source.backup(target)
        self.assertEqual(self.previous(), saved)
        self.assertFalse(any(row["job_id"] == post["job_id"] for row in self.previous()))
        with sqlite3.connect(archive) as db:
            self.assertEqual(db.execute("PRAGMA user_version").fetchone()[0], 2)
            self.assertEqual(db.execute("SELECT status,result,outcome_known FROM jobs WHERE job_id=?",
                                        (post["job_id"],)).fetchone(), (3, post["result"], 1))
        self.assertEqual(self.effect_count(), 1)
        self.previous("create")
        upgraded = self.read()
        self.assertTrue(all(len(row["generation"]) == 36 and row["outcome_known"] for row in upgraded))
        self.assertEqual(self.effect_count(), 1)

    def test_failed_upgrade_preserves_previous_reader_and_writer(self):
        before = self.previous("create")
        with sqlite3.connect(self.database) as db:
            db.execute("ALTER TABLE jobs ADD COLUMN method TEXT")
            schema = db.execute("SELECT type,name,sql FROM sqlite_master ORDER BY type,name").fetchall()
        self.assertIn("REJECTED", self.run_probe("reject"))
        self.assertEqual(self.previous(), before)
        with sqlite3.connect(self.database) as db:
            self.assertEqual(db.execute("PRAGMA user_version").fetchone()[0], 0)
            self.assertEqual(db.execute("SELECT type,name,sql FROM sqlite_master ORDER BY type,name").fetchall(), schema)
        self.assertEqual(len(self.previous("create")), len(before) + 1)
        self.assertEqual(self.effect_count(), 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
