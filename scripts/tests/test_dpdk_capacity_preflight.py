#!/usr/bin/env python3









from __future__ import annotations

import hashlib
import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch


SCRIPT = Path(__file__).resolve().parents[1] / "dpdk_capacity_preflight.py"


class CapacityCliTests(unittest.TestCase):


    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="pcv-capacity-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.sysfs = self.root / "sys"
        self.plan_path = self.root / "plan.json"
        self.plan = {
            "schema_version": 1,
            "page_size_kib": 2048,
            "ovs": {"allocation": "pending", "socket_mem_mb": "4,0", "headroom_mb": {}},
            "guests": [{"name": "vm-a", "host_node": 0, "memory_mb": 8}],
        }
        self.make_pool([(0, 10, 10, 0), (1, 10, 10, 0)])

    def make_pool(self, nodes, reserved=0):

        self.nodes = nodes
        pool = "hugepages-2048kB"
        self.global_pool = self.sysfs / "kernel/mm/hugepages" / pool
        values = {
            "nr_hugepages": sum(n[1] for n in nodes),
            "free_hugepages": sum(n[2] for n in nodes),
            "surplus_hugepages": sum(n[3] for n in nodes),
            "resv_hugepages": reserved,
        }
        self.write_counts(self.global_pool, values)
        for node, total, free, surplus in nodes:
            self.write_counts(self.sysfs / f"devices/system/node/node{node}/hugepages" / pool,
                              {"nr_hugepages": total, "free_hugepages": free,
                               "surplus_hugepages": surplus})

    @staticmethod
    def write_counts(path, values):

        path.mkdir(parents=True, exist_ok=True)
        for name, value in values.items():
            (path / name).write_text(f"{value}\n", encoding="utf-8")

    def hashes(self):

        return {str(p.relative_to(self.root)): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in self.root.rglob("*") if p.is_file()}

    def run_cli(self, raw=None):
        self.plan_path.write_text(json.dumps(self.plan) if raw is None else raw, encoding="utf-8")
        before = self.hashes()
        result = subprocess.run(
            [sys.executable, str(SCRIPT), "--plan", str(self.plan_path),
             "--sysfs-root", str(self.sysfs)], capture_output=True, text=True, check=False)
        self.assertEqual(self.hashes(), before, "CLI changed plan or sysfs files")
        self.assertEqual(result.stderr, "", result.stderr)
        return result.returncode, json.loads(result.stdout)

    def test_pending_ovs_and_guest_share_pool(self):
        code, report = self.run_cli()
        self.assertEqual(code, 0)
        self.assertEqual(report["status"], "CAPACITY-SUFFICIENT")
        node = report["nodes"][0]
        self.assertEqual((node["ovs_pages"], node["guest_pages"], node["required_pages"]), (2, 4, 6))
        self.assertTrue(report["advisory_only"])

    def test_numa_shortage_even_if_global_free_is_large(self):
        self.make_pool([(0, 4, 4, 0), (1, 100, 100, 0)])
        code, report = self.run_cli()
        self.assertEqual(code, 1)
        self.assertEqual(report["nodes"][0]["shortage_pages"], 2)

    def test_reservations_are_not_available_capacity(self):
        self.make_pool([(0, 10, 10, 0), (1, 10, 10, 0)], reserved=5)
        code, report = self.run_cli()
        self.assertEqual(code, 1)
        self.assertEqual(report["nodes"][0]["available_pages"], 5)

    def test_accounted_ovs_is_not_subtracted_twice(self):
        self.make_pool([(0, 4, 4, 0), (1, 10, 10, 0)])
        self.plan["ovs"]["allocation"] = "accounted"
        code, report = self.run_cli()
        self.assertEqual(code, 0)
        self.assertEqual(report["nodes"][0]["ovs_pages"], 0)
        self.assertEqual(report["ovs_allocation_source"], "plan")

    def test_round_each_guest_and_headroom_separately(self):
        self.plan["ovs"]["socket_mem_mb"] = "0,0"
        self.plan["ovs"]["headroom_mb"] = {"0": 1}
        self.plan["guests"] = [{"name": "vm-a", "host_node": 0, "memory_mb": 1},
                               {"name": "vm-b", "host_node": 0, "memory_mb": 1}]
        _, report = self.run_cli()
        self.assertEqual(report["nodes"][0]["required_pages"], 3)

    def test_odd_requests_cannot_fit_by_rounding_aggregate(self):
        self.make_pool([(0, 6, 6, 0), (1, 10, 10, 0)])
        self.plan["ovs"]["socket_mem_mb"] = "3,0"
        self.plan["ovs"]["headroom_mb"] = {"0": 1}
        self.plan["guests"] = [{"name": "vm-a", "host_node": 0, "memory_mb": 3},
                               {"name": "vm-b", "host_node": 0, "memory_mb": 3}]
        code, report = self.run_cli()
        self.assertEqual(code, 1)
        self.assertEqual(report["nodes"][0]["required_pages"], 7)

    def test_missing_counter_is_indeterminate(self):
        (self.global_pool / "resv_hugepages").unlink()
        code, report = self.run_cli()
        self.assertEqual((code, report["status"]), (2, "INDETERMINATE"))

    def test_malformed_counter_is_indeterminate(self):
        for value in ("-1", "12junk", "1.5", "", "9223372036854775808"):
            with self.subTest(value=value):
                (self.global_pool / "free_hugepages").write_text(value)
                code, report = self.run_cli()
                self.assertEqual((code, report["status"]), (2, "INDETERMINATE"))

    def test_global_node_drift_is_indeterminate(self):
        (self.global_pool / "free_hugepages").write_text("19\n")
        code, report = self.run_cli()
        self.assertEqual((code, report["status"]), (2, "INDETERMINATE"))

    def test_unknown_guest_node_is_indeterminate(self):
        self.plan["guests"][0]["host_node"] = 9
        code, report = self.run_cli()
        self.assertEqual((code, report["status"]), (2, "INDETERMINATE"))

    def test_other_page_sizes_do_not_cover_2m_shortage(self):
        self.make_pool([(0, 0, 0, 0), (1, 0, 0, 0)])
        self.write_counts(self.sysfs / "kernel/mm/hugepages/hugepages-1048576kB",
                          {"nr_hugepages": 200, "free_hugepages": 200, "resv_hugepages": 0,
                           "surplus_hugepages": 0})
        code, report = self.run_cli()
        self.assertEqual((code, report["status"]), (1, "CAPACITY-INSUFFICIENT"))

    def test_duplicate_json_keys_are_rejected(self):
        raw = json.dumps(self.plan).replace('"schema_version": 1', '"schema_version": 1, "schema_version": 2')
        code, report = self.run_cli(raw)
        self.assertEqual((code, report["status"]), (2, "INDETERMINATE"))

    def test_invalid_plan_values_are_rejected(self):
        for value in (True, -1, 0, 1.5, "8"):
            with self.subTest(value=value):
                self.plan["guests"][0]["memory_mb"] = value
                code, report = self.run_cli()
                self.assertEqual((code, report["status"]), (2, "INDETERMINATE"))

    def test_unsupported_page_size_is_rejected(self):
        self.plan["page_size_kib"] = 1048576
        code, report = self.run_cli()
        self.assertEqual((code, report["status"]), (2, "INDETERMINATE"))

    def test_unknown_plan_field_is_rejected(self):
        self.plan["ovs"]["headrom_mb"] = {"0": 100}
        code, report = self.run_cli()
        self.assertEqual((code, report["status"]), (2, "INDETERMINATE"))

    def test_duplicate_guest_name_is_rejected(self):
        self.plan["guests"].append(dict(self.plan["guests"][0]))
        code, report = self.run_cli()
        self.assertEqual((code, report["status"]), (2, "INDETERMINATE"))

    def test_unknown_nonzero_ovs_node_is_rejected(self):
        self.plan["ovs"]["socket_mem_mb"] = "4,0,4"
        code, report = self.run_cli()
        self.assertEqual((code, report["status"]), (2, "INDETERMINATE"))

    def test_exact_fit_and_one_page_short(self):
        self.make_pool([(0, 6, 6, 0), (1, 10, 10, 0)])
        code, _ = self.run_cli()
        self.assertEqual(code, 0)
        self.make_pool([(0, 5, 5, 0), (1, 10, 10, 0)])
        code, report = self.run_cli()
        self.assertEqual(code, 1)
        self.assertEqual(report["nodes"][0]["shortage_pages"], 1)

    def test_free_above_total_is_indeterminate(self):
        node_pool = self.sysfs / "devices/system/node/node0/hugepages/hugepages-2048kB"
        (node_pool / "nr_hugepages").write_text("1\n")
        code, report = self.run_cli()
        self.assertEqual((code, report["status"]), (2, "INDETERMINATE"))

    def test_explicit_surplus_can_back_free_pages(self):
        self.make_pool([(0, 2, 6, 4), (1, 10, 10, 0)])
        code, _ = self.run_cli()
        self.assertEqual(code, 0)

    def test_unknown_allocation_and_empty_demand_are_rejected(self):
        self.plan["ovs"]["allocation"] = "unknown"
        code, _ = self.run_cli()
        self.assertEqual(code, 2)
        self.plan["ovs"]["allocation"] = "accounted"
        self.plan["guests"] = []
        code, _ = self.run_cli()
        self.assertEqual(code, 2)

    def test_counter_change_during_read_is_indeterminate(self):

        spec = importlib.util.spec_from_file_location("capacity_probe", SCRIPT)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        original = module.read_pool
        first = True

        def change_after_read(path, global_pool=False):
            nonlocal first
            result = original(path, global_pool)
            if first:
                first = False
                (self.global_pool / "resv_hugepages").write_text("1\n")
            return result

        with patch.object(module, "read_pool", side_effect=change_after_read):
            with self.assertRaisesRegex(module.CapacityError, "changed during observation"):
                module.read_snapshot(self.sysfs)

    def test_invalid_socket_vector_is_rejected(self):
        for value in ("", "4,,0", "4,-1", "4,junk", "4, 0", "2147483648", [4, 0]):
            with self.subTest(value=value):
                self.plan["ovs"]["socket_mem_mb"] = value
                code, report = self.run_cli()
                self.assertEqual((code, report["status"]), (2, "INDETERMINATE"))


if __name__ == "__main__":
    unittest.main()
