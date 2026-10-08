#!/usr/bin/env python3








from __future__ import annotations

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


def extract_function(source: str, name: str) -> str:
    match = re.search(r"(?m)^(?:static\s+)?(?:void|gboolean)\s+(?:\n)?" + re.escape(name) + r"\([^;]*?\)\s*\{", source)
    if not match:
        raise ValueError(name)
    depth = 1
    for token in re.finditer(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|/\*.*?\*/|//[^\n]*|[{}]', source[match.end():], re.S):
        if token[0] == "{": depth += 1
        elif token[0] == "}":
            depth -= 1
            if not depth: return source[match.start():match.end()+token.end()]
    raise ValueError(name + " unterminated")


class VmStartPmWakeupTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="pcv-pm-build-")
        cls.addClassCleanup(cls.build.cleanup)
        base = Path(cls.build.name)
        cls.binary = base / "pm-wakeup"
        source = Path(os.environ.get("PCV_VM_START_PM_SOURCE", ROOT / "src/modules/dispatcher/handler_vm_start.c")).read_text()
        types = []
        for name in ("VmStartContext", "InactiveDpdkPrepare"):
            match = re.search(r"typedef struct \{(?:(?!typedef struct).)*?\} " + name + r";", source, re.S)
            if not match: raise ValueError(name)
            types.append(match[0])
        functions = ["free_vm_start_context", "_prepare_inactive_dpdk_for_start"]
        if "_wake_pm_suspended_for_start(" in source:
            functions.append("_wake_pm_suspended_for_start")
        functions += ["vm_start_worker_thread", "vm_start_callback", "handle_vm_start_request"]
        code = "\n".join(types + ["#define MAX_PHYSICAL_CPUS 256"] +
                         [extract_function(source, name) for name in functions])
        fixture = base / "fixture.c"
        template = (ROOT / "tests/fixtures/vm_start_pm_wakeup.c").read_text()
        marker = "#define PCV_PRODUCTION_FUNCTIONS"
        if template.count(marker) != 1: raise ValueError("production function marker count")
        fixture.write_text(template.replace(marker, code))
        compiler = shutil.which("gcc-14") or shutil.which("cc")
        flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "gio-2.0",
                         "json-glib-1.0", "libsoup-3.0", "libvirt", "libvirt-gobject-1.0", "sqlite3"], text=True))
        extra = shlex.split(os.environ.get("PCV_VM_START_PM_CFLAGS", ""))
        command = [compiler, "-std=gnu23", "-D_GNU_SOURCE", "-DPCV_CLUSTER_ENABLED=0", "-Wall", "-Wextra",
                   "-Werror", "-Wno-unused-parameter", "-g", "-ffunction-sections", "-fdata-sections",
                   "-I.", "-Isrc", "-Iinclude", "-Iinclude/purecvisor", str(fixture),
                   "src/modules/core/cpu_allocator.c", "src/modules/dispatcher/rpc_utils.c", "src/api/drain.c",
                   "src/modules/dispatcher/rpc_completion.c", "src/utils/pcv_validate.c",
                   "-Wl,--gc-sections", "-Wl,--wrap=cpu_allocator_allocate_exclusive",
                   "-Wl,--wrap=soup_websocket_connection_send_text", "-o", str(cls.binary), *flags, *extra]
        result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=60)
        if result.returncode: raise RuntimeError(result.stdout + result.stderr)

    def run_case(self, mode, state=7):
        with tempfile.TemporaryDirectory(prefix="pcv-pm-case-") as directory:
            base = Path(directory)
            state_path, effects, database = base/"state", base/"effects", base/"audit.db"
            state_path.write_text(f"{state}\n"); effects.write_text("")
            result = subprocess.run([str(self.binary), mode, str(state_path), str(effects), str(database)],
                                    cwd=ROOT, capture_output=True, text=True, timeout=8)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            frame = json.loads(next(line[6:] for line in result.stdout.splitlines() if line.startswith("FRAME ")))
            response = json.loads(next(line[9:] for line in result.stdout.splitlines() if line.startswith("RESPONSE ")))
            self.assertEqual(response["result"], "accepted")
            self.assertEqual(frame["type"], "job.complete")
            with sqlite3.connect(f"file:{database}?mode=ro", uri=True) as connection:
                rows = connection.execute("SELECT method,target,result,code,duration FROM audit").fetchall()
            events = effects.read_text().splitlines()
            self.assertEqual(events.count("LOCK"), 1)
            self.assertEqual(events.count("UNLOCK"), 1)
            self.assertEqual(events.count("RESPONSE"), 1)
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0][:2], ("vm.start", "11111111-2222-3333-4444-555555555555"))
            self.assertGreaterEqual(rows[0][4], 0)
            self.assertEqual(frame["payload"]["method"], "vm.start")
            print(json.dumps({"mode": mode, "initial": state, "final": int(state_path.read_text()),
                              "events": events, "audit": rows[0], "ws": frame["payload"]}), flush=True)
            return int(state_path.read_text()), events, rows[0], frame["payload"]

    def active_case(self, mode, expected_ok, initial=7, final=None):
        state, events, audit, payload = self.run_case(mode, initial)
        self.assertEqual(audit[2], "ok" if expected_ok else "fail")
        self.assertEqual(payload["status"], "ok" if expected_ok else "fail")
        self.assertEqual(audit[3], 0 if expected_ok else -32000)
        self.assertIn("COMPETITOR_DENIED", events)
        self.assertNotIn("ALLOCATE", events)
        for forbidden in ("CAPACITY", "CREATE", "DEFINE", "PIN", "HOTPLUG", "OVERLAY", "DESTROY"):
            self.assertNotIn(forbidden, events)
        if final is not None: self.assertEqual(state, final)
        return events, payload

    def test_pm_wakeup_changes_actual_state(self):
        events, _ = self.active_case("success", True, final=1)
        self.assertEqual(events.count("WAKE"), 1)
        self.assertLess(events.index("RECONCILE_ACTIVE"), events.index("WAKE"))
        self.assertLess(events.index("RESPONSE"), events.index("WAKE"))
        self.assertIn("STATE", events[events.index("WAKE")+1:])

    def test_delayed_wakeup_requires_state_confirmation(self):
        events, _ = self.active_case("delayed", True, final=1)
        self.assertEqual(events.count("WAKE"), 1)
        self.assertGreaterEqual(events.count("STATE"), 4)

    def test_wakeup_api_failure_preserves_reservation(self):
        events, payload = self.active_case("wake-error", False, final=7)
        self.assertEqual(events.count("WAKE"), 1)
        self.assertIn("fixture libvirt failure", payload["error"])

    def test_wakeup_success_without_transition_is_failure(self):
        events, payload = self.active_case("stuck", False, final=7)
        self.assertEqual(events.count("WAKE"), 1)
        self.assertIn("timed out", payload["error"].lower())

    def test_native_contract_is_in_mandatory_make_gate(self):
        source = (ROOT / "Makefile").read_text()
        gate = re.search(r"(?m)^check-dpdk-owned-lifecycle:[^\n]*\n(?P<recipe>(?:\t[^\n]*\n)+)", source)
        self.assertIsNotNone(gate)
        self.assertIn("python3 tests/test_vm_start_pm_wakeup.py", gate["recipe"])
        for target in ("check-all:", "DEV_PARALLEL_CHECKS =", "test:"):
            line = next(line for line in source.splitlines() if line.startswith(target))
            self.assertIn("check-dpdk-owned-lifecycle", line)

    def test_state_query_failure_does_not_wake(self):
        events, _ = self.active_case("state-error", False, final=7)
        self.assertNotIn("WAKE", events)

    def test_post_wakeup_query_failure_is_failure(self):
        events, _ = self.active_case("post-state-error", False, final=1)
        self.assertEqual(events.count("WAKE"), 1)

    def test_wrong_post_wakeup_state_is_failure(self):
        self.active_case("wrong-state", False, final=3)

    def test_running_is_idempotent(self):
        events, _ = self.active_case("success", True, initial=1, final=1)
        self.assertNotIn("WAKE", events)

    def test_paused_keeps_separate_resume_contract(self):
        events, _ = self.active_case("success", True, initial=3, final=3)
        self.assertNotIn("WAKE", events)

    def test_active_query_failure_preserves_reservation(self):
        events, _ = self.active_case("active-error", False, final=7)
        self.assertNotIn("WAKE", events)

    def test_active_reconcile_failure_preserves_reservation(self):
        events, _ = self.active_case("reconcile-error", False, final=7)
        self.assertNotIn("WAKE", events)

    def test_active_snapshot_becoming_inactive_is_failure(self):
        events, _ = self.active_case("inactive-race", False, final=5)
        self.assertNotIn("WAKE", events)

    def test_cold_start_keeps_capacity_prepare_and_allocation(self):
        state, events, audit, payload = self.run_case("success", 5)
        self.assertEqual(state, 1); self.assertEqual(audit[2], "ok")
        self.assertEqual(payload["status"], "ok")
        for required in ("ALLOCATE", "CAPACITY", "RECONCILE_COLD", "CREATE", "PIN", "OVERLAY", "COMPETITOR_DENIED"):
            self.assertIn(required, events)
        self.assertNotIn("WAKE", events)
        self.assertLess(events.index("CAPACITY"), events.index("CREATE"))

    def test_cold_failure_returns_allocated_cpu(self):
        state, events, audit, payload = self.run_case("cold-error", 5)
        self.assertEqual(state, 5); self.assertEqual(audit[2], "fail")
        self.assertEqual(payload["status"], "fail")
        self.assertIn("ALLOCATE", events); self.assertIn("COMPETITOR_ACQUIRED", events)
        self.assertNotIn("CREATE", events)

    def test_missing_domain_reports_failure_without_allocation(self):
        _, events, audit, payload = self.run_case("missing", 5)
        self.assertEqual(audit[2], "fail"); self.assertEqual(payload["status"], "fail")
        self.assertNotIn("ALLOCATE", events); self.assertNotIn("WAKE", events)


if __name__ == "__main__":
    unittest.main(verbosity=2)
