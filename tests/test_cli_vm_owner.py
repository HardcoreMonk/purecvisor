#!/usr/bin/env python3
                          
                                                                                                         
                                                          
                                                                                     
              
                                                        
from __future__ import annotations

import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
from test_cli_vm_usb import _ok, run_fake              

RESULT = {"name": "win", "uuid": "11111111-2222-3333-4444-555555555555", "owner": "bob",
          "previous_owner": "alice", "live_updated": True}


class CliVmOwner(unittest.TestCase):
    def test_sends_owner_set(self):
        cases = [
            (["vm", "owner", "win", "bob"], {"name": "win", "owner": "bob"}),
            (["vm", "owner", "win", "bob", "--expect", "alice"], {"name": "win", "owner": "bob", "expected_owner": "alice"}),
            (["vm", "owner", "win", "bob", "--expect-none"], {"name": "win", "owner": "bob", "expected_owner": None}),
        ]
        for args, params in cases:
            with self.subTest(args=args):
                proc, reqs = run_fake([_ok(RESULT)], args)
                self.assertEqual(proc.returncode, 0, proc.stderr)
                self.assertEqual([(r["method"], r["params"]) for r in reqs], [("vm.owner.set", params)])

    def test_usage_without_rpc(self):
        bad = [["vm", "owner", "win"], ["vm", "owner", "win", ""], ["vm", "owner", "", "bob"],
               ["vm", "owner", "win", "bob", "--expect"], ["vm", "owner", "win", "bob", "--expect", ""],
               ["vm", "owner", "win", "bob", "--expect", "a", "--expect-none"], ["vm", "owner", "win", "bob", "--force"]]
        for args in bad:
            with self.subTest(args=args):
                proc, reqs = run_fake([], args)
                self.assertEqual((proc.returncode, reqs), (2, []))
                self.assertIn("Usage: pcvctl vm owner", proc.stdout)

    def test_inspect_moved_same_request(self):
        uuid = "11111111-2222-3333-4444-555555555555"
        proc, reqs = run_fake([_ok({"uuid": uuid})], ["vm", "inspect", uuid])
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual([(r["method"], r["params"]) for r in reqs], [("vm.inspect", {"uuid": uuid})])


if __name__ == "__main__":
    unittest.main()
