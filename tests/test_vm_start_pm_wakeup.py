#!/usr/bin/env python3








from __future__ import annotations

import json
import os
from pathlib import Path
import re
import resource
import shlex
import shutil
import sqlite3
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def extract_function(source: str, name: str) -> str:
    match = re.search(r"(?m)^(?:static\s+)?(?:void|gboolean|guint|PcvVmStartSubmit)\s+(?:\n)?" + re.escape(name) + r"\([^;]*?\)\s*\{", source)
    if not match:
        raise ValueError(name)
    depth = 1
    for token in re.finditer(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|/\*.*?\*/|//[^\n]*|[{}]', source[match.end():], re.S):
        if token[0] == "{": depth += 1
        elif token[0] == "}":
            depth -= 1
            if not depth: return source[match.start():match.end()+token.end()]
    raise ValueError(name + " unterminated")

def _no_core_dump():

    try: resource.setrlimit(resource.RLIMIT_CORE, (1, 1))
    except (ValueError, OSError): resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

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
        functions += ["vm_start_worker_thread", "vm_start_callback", "pcv_vm_start_report_rejected", "_vm_start_prepare", "pcv_vm_start_submit", "handle_vm_start_request"]
        capacity = (ROOT / "src/modules/virt/vm_start_capacity.c").read_text()
        helpers = [extract_function(capacity, name) for name in ("pcv_vm_start_info_active", "pcv_vm_start_pin_count")]
        code = "\n".join(types + ["#define MAX_PHYSICAL_CPUS 256"] + helpers +
                         [extract_function(source, name) for name in functions])
        fixture = base / "fixture.c"
        template = (ROOT / "tests/fixtures/vm_start_pm_wakeup.c").read_text()
        marker = "#define PCV_PRODUCTION_FUNCTIONS"
        if template.count(marker) != 1: raise ValueError("production function marker count")
        fixture.write_text(template.replace(marker, code))
        compiler = shutil.which("gcc-14") or shutil.which("cc")
        flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "gio-2.0",
                         "json-glib-1.0", "libsoup-3.0", "libxml-2.0", "libvirt", "libvirt-gobject-1.0", "sqlite3"], text=True))
        extra = shlex.split(os.environ.get("PCV_VM_START_PM_CFLAGS", ""))
        command = [compiler, "-std=gnu23", "-D_GNU_SOURCE", "-DPCV_CLUSTER_ENABLED=0", "-Wall", "-Wextra",
                   "-Werror", "-Wno-unused-parameter", "-g", "-ffunction-sections", "-fdata-sections",
                   "-I.", "-Isrc", "-Iinclude", "-Iinclude/purecvisor", str(fixture),
                   "src/modules/core/cpu_allocator.c", "src/modules/dispatcher/rpc_utils.c", "src/api/drain.c",
                   "src/modules/dispatcher/rpc_completion.c", "src/utils/pcv_validate.c",
                   "-Wl,--gc-sections", "-Wl,--wrap=cpu_allocator_allocate_exclusive",
                   "-Wl,--wrap=cpu_allocator_free_vm_cores",
                   "-Wl,--wrap=soup_websocket_connection_send_text", "-o", str(cls.binary), *flags, *extra]
        result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=60)
        if result.returncode: raise RuntimeError(result.stdout + result.stderr)

    def run_case(self, mode, state=7):
        with tempfile.TemporaryDirectory(prefix="pcv-pm-case-") as directory:
            base = Path(directory)
            state_path, effects, database = base/"state", base/"effects", base/"audit.db"
            state_path.write_text(f"{state}\n"); effects.write_text("")
            result = subprocess.run([str(self.binary), mode, str(state_path), str(effects), str(database)],
                                    cwd=ROOT, capture_output=True, text=True, timeout=8, preexec_fn=_no_core_dump)
            self.last_stderr = result.stderr
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

    VM_UUID = "11111111-2222-3333-4444-555555555555"

    def run_submit(self, mode, state=5, driver="submit"):

        with tempfile.TemporaryDirectory(prefix="pcv-pm-submit-") as directory:
            base = Path(directory)
            state_path, effects, database = base/"state", base/"effects", base/"audit.db"
            state_path.write_text(f"{state}\n"); effects.write_text("")
            result = subprocess.run([str(self.binary), mode, str(state_path), str(effects), str(database), driver],
                                    cwd=ROOT, capture_output=True, text=True, timeout=8, preexec_fn=_no_core_dump)
            self.last_stderr = result.stderr
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            frame_text = next(line[6:] for line in result.stdout.splitlines() if line.startswith("FRAME "))
            payload = None if frame_text == "null" else json.loads(frame_text)["payload"]
            response = next(line[9:] for line in result.stdout.splitlines() if line.startswith("RESPONSE "))
            with sqlite3.connect(f"file:{database}?mode=ro", uri=True) as connection:
                rows = connection.execute("SELECT method,target,result,code FROM audit").fetchall()
            return effects.read_text().splitlines(), rows, payload, response

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

    def assert_prepare_rejected(self, mode, state):

        events, _, payload, response = self.run_submit(mode, state, driver="rpc")
        self.assertIsNone(payload)
        err = json.loads(response)["error"]
        self.assertEqual((err["code"], err["message"]), (-32000, "Hypervisor connection pool exhausted"))
        self.assertEqual(events.count("LOCK"), 1)
        self.assertEqual(events.count("UNLOCK"), 1)
        for forbidden in ("ALLOCATE", "CAPACITY", "CREATE", "WAKE", "PIN", "OVERLAY"):
            self.assertNotIn(forbidden, events)

        self.assertIn("COMPETITOR_DENIED" if state != 5 else "COMPETITOR_ACQUIRED", events)

        events, rows, payload, _ = self.run_submit(mode, state)
        self.assertIn("SUBMIT_REJECTED", events)
        self.assertNotIn("SUBMITTED", events)
        self.assertEqual(events.count("UNLOCK"), 1)
        self.assertNotIn("ALLOCATE", events)
        self.assertEqual(rows[0][2], "fail")
        self.assertEqual(payload["status"], "fail")

    def test_prepare_rejects_when_pool_gives_no_connection(self):
        self.assert_prepare_rejected("acquire-fail", 5)

    def test_prepare_rejects_on_lookup_failure_other_than_missing(self):
        self.assert_prepare_rejected("lookup-error", 5)

    def test_prepare_rejects_when_info_query_fails(self):
        self.assert_prepare_rejected("info-error-prepare", 7)

    WARN_DOMAIN = "No isolated cores for 'sleep-vm' (need 2, domain vCPUs), starting without CPU pinning"

    def test_prepare_queries_info_once_without_is_active(self):
        _, events, _, _ = self.run_case("success", 5)
        self.assertEqual(events.count("INFO"), 1)
        self.assertLess(events.index("INFO"), events.index("ALLOCATE"))
        self.assertNotIn("ACTIVE", events[:events.index("ALLOCATE")])

    def test_default_pin_count_is_domain_vcpus(self):
        _, events, audit, _ = self.run_case("vcpus-2", 5)
        self.assertIn("ALLOCATE_COUNT 2", events)
        self.assertNotIn("PIN", events)
        self.assertIn(self.WARN_DOMAIN, self.last_stderr)
        self.assertEqual(audit[2], "ok")

    def test_explicit_vcpu_count_wins_with_partial_warning(self):
        events, _, payload, _ = self.run_submit("vcpus-2", 5, driver="rpc-vcpu1")
        self.assertIn("ALLOCATE_COUNT 1", events)
        self.assertNotIn("ALLOCATE_COUNT 2", events)
        self.assertIn("PIN", events)
        self.assertIn("'sleep-vm': vcpu_count=1 pins 1 of 2 vCPUs; the rest run unpinned", self.last_stderr)
        self.assertEqual(payload["status"], "ok")

    def assert_vcpu_over_rejected(self, state):

        events, rows, payload, response = self.run_submit("vcpus-2", state, driver="rpc-vcpu3")
        self.assertEqual(rows, [])
        error = json.loads(response)["error"]
        self.assertEqual((error["code"], error["message"]), (-32602, "Invalid vcpu_count: 3 exceeds the 2 vCPUs of this VM"))
        self.assertIsNone(payload)
        self.assertEqual(events.count("LOCK"), 1)
        self.assertEqual(events.count("UNLOCK"), 1)
        self.assertEqual(events.count("RELEASE"), 1)
        self.assertEqual(events.count("FREE"), 1)
        for forbidden in ("ALLOCATE", "CREATE", "WAKE", "PIN"):
            self.assertNotIn(forbidden, events)
        return events

    def test_vcpu_count_over_domain_is_rejected(self):
        events = self.assert_vcpu_over_rejected(5)
        self.assertIn("COMPETITOR_ACQUIRED", events)

    def test_vcpu_count_over_domain_rejected_when_running(self):
        self.assert_vcpu_over_rejected(1)

    def test_invalid_vcpu_count_falls_back_to_domain_vcpus(self):
        for driver in ("rpc-vcpu-neg", "rpc-vcpu-str", "rpc-vcpu-real"):
            with self.subTest(driver=driver):
                events, _, _, _ = self.run_submit("vcpus-2", 5, driver=driver)
                self.assertIn("ALLOCATE_COUNT 2", events)
                self.assertEqual(sum(e.startswith("ALLOCATE_COUNT ") for e in events), 1)
                self.assertIn("(need 2, domain vCPUs)", self.last_stderr)
                self.assertIn(self.WARN_DOMAIN, self.last_stderr)

    def test_submit_default_is_domain_vcpus(self):
        events, _, _, _ = self.run_submit("vcpus-2", 5)
        self.assertIn("ALLOCATE_COUNT 2", events)
        self.assertEqual(sum(e.startswith("ALLOCATE_COUNT ") for e in events), 1)

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

    def test_submit_runs_same_pipeline_without_rpc_response(self):
        events, rows, payload, response = self.run_submit("success", 5)
        self.assertEqual(response, "null")
        self.assertNotIn("RESPONSE", events)
        self.assertEqual(events.count("LOCK"), 1)
        self.assertEqual(events.count("UNLOCK"), 1)
        for required in ("SUBMITTED", "ALLOCATE", "CAPACITY", "CREATE", "PIN", "OVERLAY", "DONE_OK"):
            self.assertIn(required, events)
        self.assertLess(events.index("UNLOCK"), events.index("DONE_OK"))
        self.assertEqual(rows, [("vm.start", self.VM_UUID, "ok", 0)])
        self.assertEqual(payload["method"], "vm.start")
        self.assertEqual(payload["status"], "ok")

    def test_submit_failure_still_calls_done_after_cpu_return(self):
        events, rows, payload, _ = self.run_submit("cold-error", 5)
        self.assertIn("DONE_FAIL", events)
        self.assertNotIn("DONE_OK", events)
        self.assertEqual(rows[0][2], "fail")
        self.assertEqual(payload["status"], "fail")
        self.assertIn("COMPETITOR_ACQUIRED", events)
        self.assertLess(events.index("UNLOCK"), events.index("DONE_FAIL"))

        self.assertEqual(events.count("CPU_RETURN"), 1)
        self.assertLess(events.index("CPU_RETURN"), events.index("DONE_FAIL"))

    def test_submit_lock_busy_is_reported_without_worker(self):
        events, rows, payload, response = self.run_submit("lock-busy", 5)
        self.assertIn("SUBMIT_REJECTED", events)
        for forbidden in ("LOCK", "UNLOCK", "ALLOCATE", "CAPACITY", "CREATE", "SUBMITTED", "DONE_OK", "DONE_FAIL"):
            self.assertNotIn(forbidden, events)
        self.assertEqual(rows, [("vm.start", self.VM_UUID, "fail", -32000)])
        self.assertEqual(payload["status"], "fail")
        self.assertIn("fixture: VM busy", payload["error"])
        self.assertEqual(response, "null")

    def test_submit_skip_if_active_running_is_already_active(self):
        events, rows, payload, response = self.run_submit("success", 1, "submit-skip")
        self.assertIn("ALREADY_ACTIVE", events)
        self.assertEqual(events.count("LOCK"), 1)
        self.assertEqual(events.count("UNLOCK"), 1)
        self.assertLess(events.index("LOCK"), events.index("UNLOCK"))
        for forbidden in ("ALLOCATE", "CAPACITY", "CREATE", "SUBMITTED", "SUBMIT_REJECTED", "DONE_OK", "DONE_FAIL"):
            self.assertNotIn(forbidden, events)
        self.assertEqual(rows, [])
        self.assertIsNone(payload)
        self.assertEqual(response, "null")

    def test_submit_skip_if_active_stopped_starts_normally(self):
        events, rows, payload, _ = self.run_submit("success", 5, "submit-skip")
        self.assertNotIn("ALREADY_ACTIVE", events)
        for required in ("SUBMITTED", "ALLOCATE", "CREATE", "DONE_OK"):
            self.assertIn(required, events)
        self.assertEqual(rows, [("vm.start", self.VM_UUID, "ok", 0)])
        self.assertEqual(payload["status"], "ok")

if __name__ == "__main__":
    unittest.main(verbosity=2)
