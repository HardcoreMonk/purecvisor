#!/usr/bin/env python3
                          
                                                                                                
                                                                                                     
                                                                                                             
                                                                                                       
                                                                     
                                                          
                                                                                
 
                                                                                        
                                                  
                                             
from __future__ import annotations

import csv
import io
import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
from test_cli_vm_usb import CLI, _ok, run_fake              


def _err(code, message):
    return {"jsonrpc": "2.0", "error": {"code": code, "message": message}, "id": 1}


def _vm(name, **over):
                                            
    base = {
        "name": name, "uuid": f"u-{name}", "state": "running", "quality": "ok", "reason": None, "missing": [],
        "sample_age_ms": 1200, "interval_ms": 5000.0, "last_ok_at_ms": 1794757394000, "vcpus": 2,
        "mem_max_mb": 4096.0, "mem_alloc_mb": 2048.0, "mem_guest_used_mb": 368.6, "mem_guest_available_mb": 1967.6,
        "mem_rss_mb": 510.5, "guest_stats_age_s": 5.0, "cpu_pct": 12.5, "cpu_host_pct": 3.125,
        "disk": {"rd_bps": 12500000.0, "wr_bps": 0.0, "rd_iops": 20.0, "wr_iops": 0.0, "rd_lat_ms": 2.0,
                 "wr_lat_ms": None, "fl_lat_ms": None},
        "net": {"rx_bps": 1500.0, "tx_bps": 250.0, "rx_pps": 3.0, "tx_pps": 2.0, "drop_ps": 0.0, "err_ps": 0.0},
        "contention": {"vcpu_delay_pct": 0.5, "vcpu_delay_max_pct": 0.7, "throttled_pct": 0.0,
                       "throttled_max_pct": 0.0, "throttled_periods_ratio": None, "psi": None,
                       "swap_in_kbps": 0.0, "swap_out_kbps": 0.0, "major_faults_ps": 0.0},
        "hints": [],
    }
    base.update(over)
    return base


def _result(vms, **sampler):
    s = {"state": "ok", "observed_at_ms": 1794757394000, "age_ms": 1200, "interval_ms": 5000,
         "tracked": len(vms), "truncated": 0, "error": None}
    s.update(sampler)
    return {"schema": 1, "scope": "all", "sampler": s,
            "host": {"logical_cpus": 8, "physical_cores": 4, "mem_total_mb": 16384, "allocation": {}, "psi": {}},
            "vms": vms}


NULLS = {k: None for k in ("cpu_pct", "cpu_host_pct", "mem_alloc_mb", "mem_rss_mb", "mem_guest_used_mb",
                           "mem_guest_available_mb", "guest_stats_age_s", "interval_ms", "sample_age_ms")}
NULL_VM = _vm("busy-vm", quality="busy", reason="busy", missing=["guest_memory", "rss", "disk"],
              disk={k: None for k in ("rd_bps", "wr_bps", "rd_iops", "wr_iops", "rd_lat_ms", "wr_lat_ms", "fl_lat_ms")},
              net={k: None for k in ("rx_bps", "tx_bps", "rx_pps", "tx_pps", "drop_ps", "err_ps")}, **NULLS)
HINT_VM = _vm("hot", hints=[{"code": "cpu_wait", "level": "warn", "value": 23.0, "threshold": 20.0},
                            {"code": "mem_pressure", "level": "notice", "value": 6.0, "threshold": 5.0}])


def _row(stdout, name):
    rows = [l.split() for l in stdout.splitlines() if l.split()[:1] == [name]]
    if len(rows) != 1:
        raise AssertionError(f"{name}: {len(rows)} rows in\n{stdout}")
    return rows[0]


class CliMonitorDiag(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not CLI.exists():
            raise AssertionError("missing bin/pcvctl; run make cli first")

    def test_sends_monitor_vm_diag(self):
        proc, reqs = run_fake([_ok(_result([_vm("web"), HINT_VM]))], ["monitor", "diag"])
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual([(r["method"], r["params"]) for r in reqs], [("monitor.vm.diag", {})])
        header = next(l for l in proc.stdout.splitlines() if "QUALITY" in l)
        self.assertEqual(header.split(), ["NAME", "STATE", "QUALITY", "CPU%", "HOST%", "MEM_ALLOC", "RSS",
                                          "DISK_R/W", "NET_RX/TX", "HINTS"])
        self.assertEqual(_row(proc.stdout, "web"),
                         ["web", "running", "ok", "12.5", "3.1", "2048M", "510M", "12.5M/0", "1.5K/250", "-"])
        self.assertEqual(_row(proc.stdout, "hot")[-1], "cpu_wait:warn,mem_pressure:notice")
        self.assertIn("sampler: ok, age 1200 ms, tracked 2, truncated 0, scope all", proc.stdout)
                            
        proc, _ = run_fake([_ok(_result([]))], ["monitor", "diag"])
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn(" (no VMs)", proc.stdout.splitlines())

    def test_vm_flag(self):
        proc, reqs = run_fake([_ok(_result([_vm("web")]))], ["monitor", "diag", "--vm", "web"])
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual([(r["method"], r["params"]) for r in reqs], [("monitor.vm.diag", {"vm": "web"})])

    def test_json_passthrough(self):
        res = _result([_vm("web"), NULL_VM], state="stale", age_ms=16000)
        proc, reqs = run_fake([_ok(res)], ["--format=json", "monitor", "diag"])
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(json.loads(proc.stdout)["result"], res)                   
        self.assertEqual(reqs[0]["method"], "monitor.vm.diag")

    def test_null_cells_dash(self):
        proc, _ = run_fake([_ok(_result([NULL_VM], state="error", error="connect_failed"))], ["monitor", "diag"])
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        row = _row(proc.stdout, "busy-vm")
        self.assertEqual(row, ["busy-vm", "running", "busy", "-", "-", "-", "-", "-", "-", "-"])
        self.assertNotIn("-1", proc.stdout)
        self.assertIn("sampler: error connect_failed", proc.stdout)
                                        
        proc, _ = run_fake([_ok(_result([NULL_VM, _vm("web")]))], ["--format=csv", "monitor", "diag"])
        self.assertEqual(proc.returncode, 0, proc.stderr)
        rows = list(csv.reader(io.StringIO(proc.stdout)))
        self.assertEqual(rows[0], ["name", "state", "quality", "cpu_pct", "cpu_host_pct", "mem_alloc_mb", "mem_rss_mb",
                                   "disk_rd_bps", "disk_wr_bps", "net_rx_bps", "net_tx_bps", "hints"])
        self.assertEqual(rows[1], ["busy-vm", "running", "busy", "-", "-", "-", "-", "-", "-", "-", "-", "-"])
        self.assertEqual(rows[2], ["web", "running", "ok", "12.50", "3.12", "2048", "510", "12500000", "0",
                                   "1500", "250", "-"])

    def test_unknown_arg_is_usage(self):
        for args in (["monitor", "diag", "--json"], ["monitor", "diag", "--vm"], ["monitor", "diag", "web"],
                     ["monitor", "diag", "--vm", "web", "extra"], ["monitor", "diag", "--vm", ""]):
            with self.subTest(args=args):
                proc, reqs = run_fake([], args)
                self.assertEqual((proc.returncode, reqs), (2, []), proc.stdout + proc.stderr)
                self.assertIn("Usage: pcvctl monitor diag [--vm NAME]", proc.stdout)

    def test_server_error_exit_1(self):
        proc, reqs = run_fake([_err(-32602, "vm must be a string")], ["monitor", "diag", "--vm", "web"])
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertIn("vm must be a string", proc.stderr)
        self.assertEqual(len(reqs), 1)

    def test_monitor_metrics_na(self):
                                                                      
        res = {"cpu": 12, "mem": -1, "mem_source": "unavailable"}
        proc, _ = run_fake([_ok(res)], ["monitor", "metrics", "web"])
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        mem = [l for l in proc.stdout.splitlines() if "MEMORY" in l]
        self.assertEqual(len(mem), 1, proc.stdout)
        self.assertTrue(mem[0].rstrip().endswith("N/A"), mem[0])
        self.assertNotIn("0%", mem[0])
        proc, _ = run_fake([_ok(res)], ["--format=plain", "monitor", "metrics", "web"])
        self.assertEqual(proc.stdout.splitlines()[-1], "web\t12\tN/A")

    def test_bar_negative_is_na(self):
                                                                                       
        proc, _ = run_fake([_ok({"cpu": -1, "mem": 40})], ["monitor", "metrics", "web"])
        cpu = [l for l in proc.stdout.splitlines() if "CPU" in l and "[" in l]
        self.assertEqual(len(cpu), 1, proc.stdout)
        self.assertTrue(cpu[0].rstrip().endswith("N/A"), cpu[0])
        self.assertNotIn("\u25b0", cpu[0])
        self.assertNotIn("\u25b1", cpu[0])
        mem = [l for l in proc.stdout.splitlines() if "MEMORY" in l]
        self.assertTrue(mem[0].rstrip().endswith("40%"), mem[0])

    def test_monitor_metrics_still_vm_metrics(self):
        proc, reqs = run_fake([_ok({"cpu": 12, "mem": 40})], ["--format=plain", "monitor", "metrics", "web"])
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual([(r["method"], r["params"]) for r in reqs], [("vm.metrics", {"vm_id": "web"})])
        self.assertEqual(proc.stdout.splitlines()[-2:], ["vm\tcpu\tmem", "web\t12\t40"])


if __name__ == "__main__":
    unittest.main(verbosity=1)
