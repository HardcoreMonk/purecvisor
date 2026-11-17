#!/usr/bin/env python3
                          
                                                                                                   
                                                                                 
                                                                                                    
                                                                                  
                                                                             
                                  
from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "check_prom_exposition.py"
_spec = importlib.util.spec_from_file_location("check_prom_exposition", SCRIPT)
cpe = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(cpe)

GOOD = (
    "# HELP purecvisor_host_disk_percent Host disk utilization\n"
    "# TYPE purecvisor_host_disk_percent gauge\n"
    "purecvisor_host_disk_percent 44.10\n"
    "\n# HELP purecvisor_registry PureCVisor Registry Metrics\n"
    "# TYPE node_cpu_seconds_total counter\n"
    'node_cpu_seconds_total{cpu="0",mode="idle"} 12.5\n'
    'node_cpu_seconds_total{cpu="0",mode="user"} 3\n'
    "# TYPE purecvisor_host_cpu_percent gauge\n"
    "purecvisor_host_cpu_percent 0.71339950372208816\n"
    "# TYPE purecvisor_zfs_inflight_lock_wait_ms_bucket counter\n"
    'purecvisor_zfs_inflight_lock_wait_ms_bucket{op="a",result="ok",le="+Inf"} 1\n'
    "# TYPE purecvisor_vm_diag_prom_truncated gauge\n"
    "purecvisor_vm_diag_prom_truncated NaN\n"
)
DUP_HOST = (                                            
    "# HELP purecvisor_host_cpu_percent Host CPU utilization\n"
    "# TYPE purecvisor_host_cpu_percent gauge\n"
    "purecvisor_host_cpu_percent 0.00\n"
    "\n# HELP purecvisor_registry PureCVisor Registry Metrics\n"
    "# TYPE purecvisor_host_cpu_percent gauge\n"
    "purecvisor_host_cpu_percent 0.71339950372208816\n"
)
SCATTERED = (                              
    "# TYPE node_filesystem_size_bytes gauge\n"
    'node_filesystem_size_bytes{mountpoint="/"} 1\n'
    "# TYPE node_filesystem_free_bytes gauge\n"
    'node_filesystem_free_bytes{mountpoint="/"} 1\n'
    'node_filesystem_size_bytes{mountpoint="/boot"} 2\n'
)
TOTAL_GAUGE = "# TYPE node_forks_total gauge\nnode_forks_total 7\n"
COUNTER_NO_TOTAL = "# TYPE purecvisor_x_bytes counter\npurecvisor_x_bytes 7\n"


class PromExpositionContract(unittest.TestCase):
    def test_good(self):
        self.assertEqual(cpe.check(GOOD), [])

    def test_duplicate_host_shape(self):
        errs = cpe.check(DUP_HOST)
        self.assertTrue(any("TYPE 이 두 번" in e for e in errs), errs)
        self.assertTrue(any("같은 (이름, 라벨)" in e for e in errs), errs)

    def test_scattered(self):
        errs = cpe.check(SCATTERED)
        self.assertEqual(len(errs), 1, errs)
        self.assertIn("한 덩어리가 아니다", errs[0])

    def test_total_gauge(self):
        self.assertEqual(cpe.check(TOTAL_GAUGE), ["node_forks_total: `_total` 이름인데 TYPE gauge"])

    def test_counter_without_total(self):
        errs = cpe.check(COUNTER_NO_TOTAL)
        self.assertEqual(len(errs), 1, errs)
        self.assertIn("`_total` 로 끝나지 않는다", errs[0])

    def test_escaped_label_values(self):
                                                                                   
        ok = '# TYPE v gauge\nv{vm="q\\"uo\\\\te"} 1\nv{vm="nl\\nx}y,z=\\"w\\""} 2\n'
        self.assertEqual(cpe.check(ok), [])
        self.assertTrue(cpe.check('# TYPE v gauge\nv{vm="q"uo"} 1\n'))
        self.assertTrue(cpe.check('# TYPE v gauge\nv{mountpoint="/mnt/a\\040b"} 1\n'))

    def test_line_syntax(self):
        self.assertTrue(cpe.check('# TYPE a gauge\na{x="1} 1\n'))
        self.assertTrue(cpe.check("# TYPE a gauge\na one\n"))

    def test_cli_jsonl_and_exit_codes(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "head.jsonl"
            recs = [{"path": "/metrics", "probe": "direct", "status": 200, "headers": {}, "body": GOOD},
                    {"path": "/metrics", "probe": "forwarded", "status": 401, "headers": {}, "body": "{}"}]
            p.write_text("".join(json.dumps(r) + "\n" for r in recs), encoding="utf-8")
            ok = subprocess.run([sys.executable, str(SCRIPT), "--jsonl", str(p)], capture_output=True, text=True)
            self.assertEqual(ok.returncode, 0, ok.stdout + ok.stderr)
            p.write_text(json.dumps(dict(recs[0], body=DUP_HOST)) + "\n", encoding="utf-8")
            bad = subprocess.run([sys.executable, str(SCRIPT), "--jsonl", str(p)], capture_output=True, text=True)
            self.assertEqual(bad.returncode, 1, bad.stdout)
            none = subprocess.run([sys.executable, str(SCRIPT), "--jsonl", str(p), "--probe", "token"],
                                  capture_output=True, text=True)
            self.assertEqual(none.returncode, 2, none.stdout)
        st = subprocess.run([sys.executable, str(SCRIPT)], capture_output=True, text=True)
        self.assertEqual(st.returncode, 0, st.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=1)
