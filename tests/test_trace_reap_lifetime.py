#!/usr/bin/env python3









from __future__ import annotations

import json
import os
import select
import signal
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class TraceReapLifetimeTests(unittest.TestCase):


    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="pcv-trace-reap-build-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.build = Path(cls.temp.name)
        cls.binary = cls.build / "trace-lifetime"
        compiler = shutil.which("gcc-14") or shutil.which("cc")
        flags = shlex.split(subprocess.check_output(
            ["pkg-config", "--cflags", "--libs", "gio-2.0", "json-glib-1.0", "libvirt"], text=True))
        source = os.environ.get("PCV_TRACE_LIFETIME_SOURCE", "")
        extra = shlex.split(os.environ.get("PCV_TRACE_LIFETIME_CFLAGS", ""))
        source_flag = [f'-DPCV_TRACE_SOURCE="{Path(source).resolve()}"'] if source else []
        command = [compiler, "-std=gnu23", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
                   "-Wno-unused-parameter", "-g", "-ffunction-sections", "-fdata-sections",
                   "-Isrc", "-Iinclude", "tests/fixtures/trace_reap_lifetime.c",
                   "src/utils/pcv_validate.c", "-Wl,--gc-sections",
                   "-Wl,--wrap=g_subprocess_force_exit", "-Wl,--wrap=g_subprocess_wait_finish",
                   "-o", str(cls.binary), *source_flag, *flags, *extra]
        result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        (cls.build / "retis").symlink_to(cls.binary)
        if os.environ.get("PCV_TRACE_LIFETIME_VALGRIND"):

            native = cls.binary
            cls.binary = cls.build / "trace-lifetime-valgrind"
            log = str(Path(os.environ["PCV_TRACE_LIFETIME_VALGRIND"]).resolve())
            command = ["valgrind", "--leak-check=full", "--show-leak-kinds=definite,indirect",
                       "--errors-for-leak-kinds=definite,indirect", "--error-exitcode=99",
                       f"--log-file={log}.%p.log", str(native)]
            cls.binary.write_text("#!/bin/sh\nexec " + shlex.join(command) + ' "$@"\n')
            cls.binary.chmod(0o700)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="pcv-trace-reap-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def read_phase(self, proc, phase):
        self.assertTrue(select.select([proc.stdout], [], [], 5)[0], f"missing {phase}")
        text = proc.stdout.readline()
        self.assertTrue(text, f"fixture ended before {phase}")
        value = json.loads(text)
        self.assertEqual(value["phase"], phase)

        value["observed_marker"] = Path(value["marker"]).exists()
        value["observed_pid_exists"] = Path(f'/proc/{value["pid"]}').exists()
        audit = self.root / "audit.jsonl"
        value["observed_audits"] = [json.loads(line) for line in audit.read_text().splitlines()]
        return value

    def run_case(self, mode):
        env = os.environ.copy()
        env["PATH"] = str(self.build) + os.pathsep + env["PATH"]
        env["PCV_TRACE_CHILD_MODE"] = {"natural": "natural", "error-exit": "error",
                                       "already-reaped": "exit"}.get(mode, "hang")
        env.pop("G_DEBUG", None)
        proc = subprocess.Popen([str(self.binary), str(self.root), mode], cwd=ROOT, env=env,
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, text=True, bufsize=1)
        observations = []
        child_handle = None
        try:
            observations.append(self.read_phase(proc, "before"))


            if mode not in ("already-reaped", "natural", "error-exit"):
                child_handle = os.pidfd_open(observations[0]["pid"])
            proc.stdin.write("request\n"); proc.stdin.flush()
            observations.append(self.read_phase(proc, "pending"))
            proc.stdin.write("drain\n"); proc.stdin.flush()
            observations.append(self.read_phase(proc, "after"))
            proc.stdin.write("cleanup\n"); proc.stdin.flush()
            out, err = proc.communicate(timeout=5)
            self.assertEqual(proc.returncode, 0, out + err)
            return observations, err
        finally:
            if proc.poll() is None:
                proc.kill(); proc.communicate(timeout=5)

            if child_handle is not None:
                try:
                    signal.pidfd_send_signal(child_handle, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                os.close(child_handle)
            for stream in (proc.stdin, proc.stdout, proc.stderr):
                stream.close()

    def assert_pending(self, pending, requested=True):
        self.assertTrue(pending["observed_marker"])
        self.assertFalse(pending["guard_available"])
        self.assertEqual(pending["state"], "running")
        self.assertEqual(pending.get("stop_requested", False), requested)
        self.assertEqual(len(pending["observed_audits"]), 1)
        self.assertEqual(pending["criticals"], 0)

    def assert_complete(self, after, method="debug.trace.stop", result="fail"):
        self.assertFalse(after["observed_marker"])
        self.assertFalse(after["observed_pid_exists"])
        self.assertTrue(after["guard_available"])
        self.assertEqual(after["state"], "idle")
        self.assertEqual(after["criticals"], 0)
        self.assertGreater(after["wait_calls"], 0)
        self.assertEqual(len(after["observed_audits"]), 2)
        self.assertEqual(after["observed_audits"][-1]["method"], method)
        self.assertEqual(after["observed_audits"][-1]["result"], result)

    def test_stop_keeps_marker_guard_until_wait_confirmation(self):
        (before, pending, after), log = self.run_case("stop")
        self.assertTrue(before["observed_pid_exists"])
        self.assert_pending(pending)
        self.assert_complete(after)

    def test_backstop_keeps_marker_guard_until_wait_confirmation(self):
        (before, pending, after), log = self.run_case("backstop")
        self.assertTrue(before["observed_pid_exists"])
        self.assert_pending(pending)
        self.assert_complete(after, method="debug.trace.expire")

    def test_repeated_stop_sends_signal_once(self):
        (_, pending, after), log = self.run_case("repeat-stop")
        self.assertTrue(pending["action_ok"])
        self.assert_pending(pending)
        self.assertEqual(after["force_calls"], 1)
        self.assert_complete(after)

    def test_first_stop_reason_wins_over_backstop(self):
        (_, pending, after), log = self.run_case("stop-then-backstop")
        self.assert_pending(pending)
        self.assertEqual(after["force_calls"], 1)
        self.assert_complete(after, method="debug.trace.stop")

    def test_first_expire_reason_wins_over_stop(self):
        (_, pending, after), log = self.run_case("backstop-then-stop")
        self.assertTrue(pending["action_ok"])
        self.assert_pending(pending)
        self.assertEqual(after["force_calls"], 1)
        self.assert_complete(after, method="debug.trace.expire")

    def test_already_reaped_child_still_waits_for_its_callback(self):
        (before, pending, after), log = self.run_case("already-reaped")
        self.assertFalse(before["observed_pid_exists"])
        self.assert_pending(pending)
        self.assert_complete(after, result="ok")

    def test_natural_zero_exit_is_positive_control(self):
        (_, pending, after), log = self.run_case("natural")
        self.assert_pending(pending, requested=False)
        self.assertEqual(after["force_calls"], 0)
        self.assert_complete(after, result="ok")

    def test_natural_error_exit_is_not_success(self):
        (_, pending, after), log = self.run_case("error-exit")
        self.assert_pending(pending, requested=False)
        self.assert_complete(after)

    def test_wrong_trace_id_does_not_touch_active_child(self):
        (_, pending, after), log = self.run_case("wrong-id")
        self.assertFalse(pending["action_ok"])
        self.assertTrue(pending["observed_pid_exists"])
        self.assertEqual(pending["force_calls"], 0)
        self.assert_pending(pending, requested=False)
        self.assert_complete(after)

    def test_empty_trace_id_does_not_touch_active_child(self):
        (_, pending, after), log = self.run_case("empty-id")
        self.assertFalse(pending["action_ok"])
        self.assert_pending(pending, requested=False)
        self.assert_complete(after)

    def test_stale_process_callback_cannot_finalize_current_trace(self):
        (_, pending, after), log = self.run_case("stale-callback")
        self.assertTrue(pending["observed_pid_exists"])
        self.assert_pending(pending, requested=False)
        self.assert_complete(after)

    def test_wait_failure_keeps_unconfirmed_completion_guarded(self):
        (_, pending, after), log = self.run_case("wait-error")
        self.assert_pending(pending)
        self.assert_pending(after)
        self.assertFalse(after["observed_pid_exists"])
        self.assertEqual(after["wait_calls"], 1)
        self.assertIn("wait", log)


if __name__ == "__main__":
    unittest.main()
