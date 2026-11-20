#!/usr/bin/env python3

from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
from test_cli_vm_usb import CLI, _ok, run_fake

ID1 = "0123456789abcdef0123456789abcdef"
ID2 = "fedcba9876543210fedcba9876543210"

def _entry(eid, **over):
    base = {"id": eid, "vm_uuid": "6f1c2d3e-4a5b-4c6d-8e9f-0a1b2c3d4e5f", "vm_name": "web", "observed_at": 1795100000000,
            "window_start": None, "kind": "changed", "cause": "rpc", "scope": "config", "attribution": "rpc",
            "actor": "alice", "actor_hidden": False, "actor_expired": False, "actor_method": "vm.set_vcpu",
            "actor_result": "ok", "change_count": 2,
            "changes": [{"path": "vcpu.current", "before": "2", "after": "4"}, {"path": "vcpu.max", "before": "4", "after": "8"}]}
    base.update(over)
    return base

def _list(entries, **extra):
    r = {"entries": entries, "gaps": [], "store": {"state": "ok", "oldest_at": None, "pruned_before": None,
                                                    "retention_days": 365},
         "scope": "all", "next_cursor": None}
    r.update(extra)
    return r

def _err(code, message):
    return {"jsonrpc": "2.0", "error": {"code": code, "message": message}, "id": 1}

def _row(stdout, prefix):
    rows = [l.split() for l in stdout.splitlines() if l.split()[:1] == [prefix]]
    if len(rows) != 1:
        raise AssertionError(f"{prefix}: {len(rows)} rows in\n{stdout}")
    return rows[0]

class CliVmConfigHistory(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not CLI.exists():
            raise AssertionError("missing bin/pcvctl; run make cli first")

    def test_table_columns_and_markers(self):
        res = _list([_entry(ID1),
                     _entry(ID2, actor=None, actor_hidden=True, window_start=1795099000000, cause="reconcile",
                            attribution="rpc", change_count=1, changes=[{"path": "memory.max_kib", "before": 1, "after": 2}]),
                     _entry("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", actor=None, actor_expired=True),
                     _entry("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", actor=None, attribution="none", kind="baseline",
                            change_count=0, changes=[])])
        proc, reqs = run_fake([_ok(res)], ["vm", "config-history", "--vm", "web", "--limit", "20"])
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual([(r["method"], r["params"]) for r in reqs],
                         [("vm.config.history.list", {"vm": "web", "limit": 20})])
        header = next(l for l in proc.stdout.splitlines() if "CAUSE" in l)
        self.assertEqual(header.split(), ["ID", "TIME", "VM", "KIND", "CAUSE", "ACTOR", "CHANGES"])
        r1 = _row(proc.stdout, "01234567…")
        self.assertEqual(r1[3:7], ["web", "changed", "rpc", "alice"])
        self.assertEqual(r1[7:], ["2", "vcpu.current,…"])
        r2 = _row(proc.stdout, "fedcba98…")
        self.assertTrue(r2[1].startswith("~"), r2)
        self.assertEqual(r2[6], "(hidden)")
        self.assertEqual(_row(proc.stdout, "aaaaaaaa…")[6], "(expired)")
        rb = _row(proc.stdout, "bbbbbbbb…")
        self.assertEqual(rb[6], "-")
        self.assertNotIn("null", proc.stdout)
        self.assertIn("store: ok, gaps 0", proc.stdout)

    def test_time_args_ms_and_iso(self):
        proc, reqs = run_fake([_ok(_list([]))], ["vm", "config-history", "--uuid", "6f1c2d3e-4a5b-4c6d-8e9f-0a1b2c3d4e5f",
                                                 "--since", "1795000000000", "--until", "2026-11-20T00:00:00+00:00"])
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(reqs[0]["params"], {"uuid": "6f1c2d3e-4a5b-4c6d-8e9f-0a1b2c3d4e5f", "since": 1795000000000,
                                             "until": 1795132800000})
        self.assertIn("(no configuration changes in range)", proc.stdout)

    def test_json_passthrough(self):
        res = _list([_entry(ID1, actor=None, actor_hidden=True)], next_cursor="abc=")
        proc, reqs = run_fake([_ok(res)], ["--format=json", "vm", "config-history"])
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(json.loads(proc.stdout)["result"], res)
        self.assertEqual(reqs[0], {**reqs[0], "method": "vm.config.history.list"})

    def test_show(self):
        res = dict(_entry(ID1), snapshot={"config": {}, "live_overlay": None, "autostart": False}, prev_id=None,
                   changes=[{"path": "opaque.qemu_commandline", "before": None, "after": "ab12"},
                            {"path": "owner", "before": "(다른 소유자)", "after": "alice"}])
        proc, reqs = run_fake([_ok(res)], ["vm", "config-history-show", ID1])
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual([(r["method"], r["params"]) for r in reqs], [("vm.config.history.get", {"id": ID1})])
        self.assertEqual(_row(proc.stdout, "opaque.qemu_commandline"), ["opaque.qemu_commandline", "-", "ab12"])
        self.assertIn("ACTOR  : alice (rpc)", proc.stdout)

    def test_usage_exit_2(self):
        for args in (["vm", "config-history", "--vm"], ["vm", "config-history", "--limit", "0"],
                     ["vm", "config-history", "--limit", "201"], ["vm", "config-history", "--since", "yesterday"],
                     ["vm", "config-history", "web"], ["vm", "config-history-show"],
                     ["vm", "config-history-show", "ABCDEF0123456789ABCDEF0123456789"],
                     ["vm", "config-history-show", ID1, "extra"]):
            with self.subTest(args=args):
                proc, reqs = run_fake([], args)
                self.assertEqual((proc.returncode, reqs), (2, []), proc.stdout + proc.stderr)
                self.assertIn("Usage: pcvctl vm config-history", proc.stdout)

    def test_server_error_exit_1(self):
        proc, reqs = run_fake([_err(-32006, "Permission denied: configuration change not found or not visible")],
                              ["vm", "config-history-show", ID2])
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertIn("not found or not visible", proc.stderr)
        self.assertEqual(len(reqs), 1)

if __name__ == "__main__":
    sys.exit(0 if unittest.main(exit=False, verbosity=1).result.wasSuccessful() else 1)
