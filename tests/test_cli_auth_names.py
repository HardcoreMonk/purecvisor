#!/usr/bin/env python3
                          
                                                                                                                           
                                                                                                       
                                                                                                 
                                                                                                         
                                                                         
                                                                                            
                                                                              
 
                                                                         
                                                                
                                        
from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
from test_cli_vm_usb import CLI, _ok, run_fake              

PW = "Cli-Names-Pw-73!"
CREATE_USAGE = "Usage: pcvctl auth create <username> <password> <role> [--reuse-deleted-name]"
RETIRE_USAGE = "Usage: pcvctl auth retire-name <name>"


def defines(path: str, name: str) -> bool:
                                                
    text = (ROOT / path).read_text()
    return re.search(r"^void\s+" + re.escape(name) + r"\s*\([^;{]*\)\s*\{", text, re.M) is not None


class CliAuthNames(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not CLI.exists():
            raise AssertionError("missing bin/pcvctl; run make cli first")

    def test_create_params_unchanged(self):                     
        proc, reqs = run_fake([_ok({"username": "alice", "status": "created"})], ["auth", "create", "alice", PW, "viewer"])
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual([r["method"] for r in reqs], ["auth.user.create"])
        self.assertEqual(reqs[0]["params"], {"username": "alice", "password": PW, "role": "viewer"})

    def test_create_reuse_flag(self):                                                   
        for argv in (["--reuse-deleted-name", "alice", PW, "admin"], ["alice", "--reuse-deleted-name", PW, "admin"],
                     ["alice", PW, "--reuse-deleted-name", "admin"], ["alice", PW, "admin", "--reuse-deleted-name"]):
            with self.subTest(argv=argv):
                proc, reqs = run_fake([_ok({"username": "alice", "reused_deleted_name": True})], ["auth", "create", *argv])
                self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
                self.assertEqual(reqs[0]["params"], {"username": "alice", "password": PW, "role": "admin",
                                                     "reuse_deleted_name": True})

    def test_create_usage_exit(self):                                               
        for argv in ([], ["alice"], ["alice", PW], ["alice", PW, "--reuse-deleted-name"]):
            with self.subTest(argv=argv):
                proc, reqs = run_fake([], ["auth", "create", *argv])
                self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)
                self.assertEqual(reqs, [])
                self.assertIn(CREATE_USAGE, proc.stdout)

    def test_create_rpc_error(self):                                 
        err = {"jsonrpc": "2.0", "id": 1, "error": {"code": -32002, "message": "username belongs to a deleted account"}}
        proc, reqs = run_fake([err], ["auth", "create", "bob", PW, "viewer"])
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertEqual(reqs[0]["method"], "auth.user.create")
        self.assertIn("username belongs to a deleted account", proc.stdout + proc.stderr)

    def test_retire_name_params(self):                                      
        proc, reqs = run_fake([_ok({"username": "gone", "added": True})], ["auth", "retire-name", "gone"])
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual([r["method"] for r in reqs], ["auth.deleted_name.add"])
        self.assertEqual(reqs[0]["params"], {"username": "gone"})

    def test_retire_name_usage(self):                              
        for argv in ([], ["a", "b"]):
            with self.subTest(argv=argv):
                proc, reqs = run_fake([], ["auth", "retire-name", *argv])
                self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)
                self.assertEqual(reqs, [])
                self.assertIn(RETIRE_USAGE, proc.stdout)

    def test_retire_name_rpc_error(self):                 
        err = {"jsonrpc": "2.0", "id": 1,
               "error": {"code": -32002, "message": "username belongs to an existing account — delete the account instead"}}
        proc, reqs = run_fake([err], ["auth", "retire-name", "alice"])
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertEqual(reqs[0]["method"], "auth.deleted_name.add")
        self.assertIn("existing account", proc.stdout + proc.stderr)

    def test_create_moved(self):                                               
        self.assertTrue(defines("src/cli/cli_auth_names.c", "cmd_auth_create"))
        self.assertTrue(defines("src/cli/cli_auth_names.c", "cmd_auth_retire_name"))
        self.assertFalse(defines("src/cli/purecvisorctl.c", "cmd_auth_create"))
        ctl = (ROOT / "src/cli/purecvisorctl.c").read_text()
        self.assertIn('{"auth","retire-name", cmd_auth_retire_name, "Retire a deleted user name without an account (admin)"}', ctl)
        self.assertIn('#include "cli_auth_names.h"', ctl)


if __name__ == "__main__":
    unittest.main()
